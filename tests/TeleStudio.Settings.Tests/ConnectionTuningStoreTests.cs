using TeleStudio.Contracts;

namespace TeleStudio.Settings.Tests;

public sealed class ConnectionTuningStoreTests
{
    [Theory]
    [InlineData(CallProtocol.Sip)]
    [InlineData(CallProtocol.H323)]
    public async Task Opal_profiles_prefer_1080p_for_both_families_and_saved_explicit_limits_survive(CallProtocol protocol)
    {
        var root = Directory.CreateTempSubdirectory("Opal-video-defaults-");
        try
        {
            var store = new ConnectionTuningStore(root.FullName);
            var fresh = await store.LoadAsync(EngineFamily.Cisco, protocol, default, missingDefaults: OpalEditionDefaults.Tuning(EngineFamily.Cisco));
            Assert.Equal(VideoResolution.Fhd, fresh.CameraResolution);
            Assert.Equal(VideoResolution.Fhd, fresh.ScreenResolution);
            Assert.Equal(4000, fresh.VideoCeilingKbps);
            Assert.Equal(VideoResolution.Fhd, OpalEditionDefaults.Settings(EngineFamily.Cisco).VideoResolution);
            Assert.Equal(4000, OpalEditionDefaults.Settings(EngineFamily.Cisco).VideoBitrateKbps);
            var explicitHd = fresh with { CameraResolution = VideoResolution.Hd, VideoCeilingKbps = 1600 };
            await store.SaveAsync(EngineFamily.Cisco, protocol, explicitHd, default);
            Assert.Equal(explicitHd, await store.LoadAsync(EngineFamily.Cisco, protocol, default, missingDefaults: OpalEditionDefaults.Tuning(EngineFamily.Cisco)));
            Assert.Equal(VideoResolution.Fhd, OpalEditionDefaults.Settings(EngineFamily.Sony).VideoResolution);
            var sony = await store.LoadAsync(EngineFamily.Sony, protocol, default, missingDefaults: OpalEditionDefaults.Tuning(EngineFamily.Sony));
            Assert.Equal(VideoResolution.Fhd, sony.CameraResolution);
            Assert.Equal(4000, sony.VideoCeilingKbps);
        }
        finally { root.Delete(true); }
    }

    [Fact]
    public async Task Aac_enabled_survives_save_and_is_isolated_by_family_and_protocol()
    {
        var root = Directory.CreateTempSubdirectory("Opal-tuning-");
        try
        {
            var store = new ConnectionTuningStore(root.FullName);
            var tuning = System.Text.Json.JsonSerializer.Deserialize<ConnectionTuning>("{\"AacEnabled\":true}")!;
            await store.SaveAsync(EngineFamily.Cisco, CallProtocol.H323, tuning, default);
            var saved = System.Text.Json.JsonSerializer.Serialize(await store.LoadAsync(EngineFamily.Cisco, CallProtocol.H323, default));
            Assert.Contains("\"AacEnabled\":true", saved);
            foreach (var (family, protocol) in new[] { (EngineFamily.Sony, CallProtocol.H323), (EngineFamily.Cisco, CallProtocol.Sip) })
                Assert.Contains("\"AacEnabled\":false", System.Text.Json.JsonSerializer.Serialize(await store.LoadAsync(family, protocol, default)));
        }
        finally { root.Delete(true); }
    }

    [Fact]
    public async Task Missing_profile_migrates_legacy_external_without_overwriting_saved_profiles()
    {
        var root = Path.Combine(Path.GetTempPath(), "EssaStudio-tuning-" + Guid.NewGuid().ToString("N"));
        try
        {
            var store = new ConnectionTuningStore(root);
            var migrated = await store.LoadAsync(EngineFamily.Cisco, CallProtocol.Sip, default, ConnectionPreset.External);
            Assert.Equal(100, migrated.SendBufferMs);
            await store.SaveAsync(EngineFamily.Cisco, CallProtocol.Sip, new() { SendBufferMs = 60 }, default);
            Assert.Equal(60, (await store.LoadAsync(EngineFamily.Cisco, CallProtocol.Sip, default, ConnectionPreset.External)).SendBufferMs);
            Assert.Equal(40, (await store.LoadAsync(EngineFamily.Sony, CallProtocol.Sip, default)).SendBufferMs);
            Assert.Equal(40, (await store.LoadAsync(EngineFamily.Cisco, CallProtocol.H323, default)).SendBufferMs);
        }
        finally { if (Directory.Exists(root)) Directory.Delete(root, true); }
    }
}
