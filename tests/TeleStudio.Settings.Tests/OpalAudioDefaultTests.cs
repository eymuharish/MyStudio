using TeleStudio.Contracts;
namespace TeleStudio.Settings.Tests;
public sealed class OpalAudioDefaultTests
{
    [Fact]
    public void Video_settings_migration_preserves_devices_and_later_video_choice()
    {
        var previous = new AppSettings { VideoResolution = VideoResolution.Hd, VideoBitrateKbps = 1500,
            MicrophoneId = "chosen mic", LecturerName = "Lecturer" };
        var migrated = OpalEditionDefaults.UpgradeVideoSettings(previous);
        Assert.Equal(VideoResolution.Fhd, migrated.VideoResolution); Assert.Equal(4000, migrated.VideoBitrateKbps);
        Assert.Equal("chosen mic", migrated.MicrophoneId); Assert.Equal("Lecturer", migrated.LecturerName);
        var chosen = AppSettingsCopy.CopyVideo(migrated, VideoResolution.Hd, 1500, 1);
        Assert.Equal(chosen, OpalEditionDefaults.UpgradeVideoSettings(chosen));
        Assert.Equal(chosen.OpalVideoDefaultsVersion, chosen.WithRecentDestination("192.0.2.22").OpalVideoDefaultsVersion);
    }
    [Theory]
    [InlineData(EngineFamily.Cisco)]
    [InlineData(EngineFamily.Sony)]
    public void Video_defaults_prefer_1080p_4mbps_and_preserve_later_explicit_limits(EngineFamily family)
    {
        var tuning = OpalEditionDefaults.Tuning(family);
        Assert.Equal(VideoResolution.Fhd, tuning.CameraResolution);
        Assert.Equal(VideoResolution.Fhd, tuning.ScreenResolution);
        Assert.Equal(4000, tuning.VideoCeilingKbps);
        Assert.Equal(30, tuning.CameraFps); Assert.Equal(30, tuning.ScreenFps);
        var migrated = OpalEditionDefaults.UpgradeVideoDefaults(new() { VideoCeilingKbps = 1500 });
        Assert.Equal(4000, migrated.VideoCeilingKbps);
        var chosen = migrated with { VideoCeilingKbps = 1500, CameraResolution = VideoResolution.Hd };
        Assert.Equal(chosen, OpalEditionDefaults.UpgradeVideoDefaults(chosen));
    }
    [Theory]
    [InlineData(EngineFamily.Cisco, AudioSelection.AacLd, 128)]
    [InlineData(EngineFamily.Sony, AudioSelection.AacLc, 192)]
    public void Fresh_opal_settings_offer_aac_stereo(EngineFamily family, AudioSelection codec, int kbps)
    {
        var tuning = OpalEditionDefaults.Tuning(family);
        Assert.True(tuning.AacEnabled);
        Assert.Equal(codec, tuning.AudioCodec);
        Assert.Equal(2, tuning.AudioChannels);
        Assert.Equal(kbps, tuning.AudioKbps);
    }
    [Fact]
    public void Upgrade_changes_audio_defaults_once_and_preserves_other_preferences()
    {
        var previous = new ConnectionTuning { AacEnabled = false, AudioCodec = AudioSelection.G722,
            JitterMaximumMs = 600, SendBufferMs = 150, VideoCeilingKbps = 768, ScreenFps = 5 };
        var upgraded = OpalEditionDefaults.UpgradeAudioDefaults(previous, EngineFamily.Cisco);
        Assert.True(upgraded.AacEnabled); Assert.Equal(2, upgraded.AudioChannels);
        Assert.Equal(600, upgraded.JitterMaximumMs); Assert.Equal(150, upgraded.SendBufferMs);
        Assert.Equal(768, upgraded.VideoCeilingKbps); Assert.Equal(5, upgraded.ScreenFps);
        var explicitOff = upgraded with { AacEnabled = false, AudioCodec = AudioSelection.G722, AudioChannels = 1, AudioKbps = 64 };
        Assert.Equal(explicitOff, OpalEditionDefaults.UpgradeAudioDefaults(explicitOff, EngineFamily.Cisco));
    }
    [Fact]
    public async Task An_explicit_later_mono_choice_survives_storage_and_upgrade()
    {
        var dir = Directory.CreateTempSubdirectory();
        try {
            var store = new ConnectionTuningStore(dir.FullName);
            var chosen = OpalEditionDefaults.Tuning(EngineFamily.Sony) with { AudioChannels = 1, AudioKbps = 96 };
            await store.SaveAsync(EngineFamily.Sony, CallProtocol.H323, chosen, default);
            var loaded = await store.LoadAsync(EngineFamily.Sony, CallProtocol.H323, default);
            Assert.Equal(chosen, OpalEditionDefaults.UpgradeAudioDefaults(loaded, EngineFamily.Sony));
        } finally { dir.Delete(true); }
    }
}
