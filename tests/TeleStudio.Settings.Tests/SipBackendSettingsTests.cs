using TeleStudio.Contracts;
using TeleStudio.Settings;

namespace TeleStudio.Settings.Tests;

public sealed class SipBackendSettingsTests
{
    [Fact]
    public async Task Opal_selection_survives_save_load_and_remembering_a_destination()
    {
        var directory = Directory.CreateTempSubdirectory("Opal-settings-roundtrip-");
        try
        {
            var path = Path.Combine(directory.FullName, "settings.json");
            var store = new SettingsStore(path);
            var settings = new AppSettings { SipEngine = SipEngineKind.Opal, VideoResolution = VideoResolution.Fhd,
                VideoBitrateKbps = 4000, CameraId = "selected-camera" };
            await store.SaveAsync(settings, default);
            Assert.Contains("\"sipEngine\": \"opal\"", await File.ReadAllTextAsync(path));
            var loaded = await store.LoadAsync(default);
            Assert.Equal(settings, loaded);
            await store.SaveAsync(loaded.WithRecentDestination("127.0.0.1:5141"), default);
            var remembered = await store.LoadAsync(default);
            Assert.Equal(SipEngineKind.Opal, remembered.SipEngine);
            Assert.Equal(VideoResolution.Fhd, remembered.VideoResolution);
            Assert.Equal(4000, remembered.VideoBitrateKbps);
            Assert.Equal("selected-camera", remembered.CameraId);
            Assert.Equal(new[] { "127.0.0.1:5141" }, remembered.RecentDestinations);
            Assert.Empty(Directory.GetFiles(directory.FullName, "*.corrupt-*"));
        }
        finally { directory.Delete(true); }
    }

    [Theory]
    [InlineData("\"pjsip\"")]
    [InlineData("\"liblinphone\"")]
    [InlineData("0")]
    [InlineData("1")]
    [InlineData("2")]
    public async Task Retired_backend_migrates_to_opal_without_losing_existing_settings(string backend)
    {
        var directory = Path.Combine(Path.GetTempPath(), Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        try
        {
            var path = Path.Combine(directory, "settings.json");
            var document = "{\"sipEngine\":" + backend + ",\"language\":\"ar\",\"cameraId\":\"selected-camera\",\"microphoneId\":\"selected-mic\",\"speakerId\":\"selected-speaker\",\"roomVolume\":37,\"microphoneGain\":123,\"recentDestinations\":[\"10.1.2.3\"]}";
            await File.WriteAllTextAsync(path, document);
            var store = new SettingsStore(path);
            var loaded = await store.LoadAsync(CancellationToken.None);
            Assert.Equal(SipEngineKind.Opal, loaded.SipEngine);
            Assert.Equal("ar", loaded.Language);
            Assert.Equal("selected-camera", loaded.CameraId);
            Assert.Equal("selected-mic", loaded.MicrophoneId);
            Assert.Equal("selected-speaker", loaded.SpeakerId);
            Assert.Equal(37, loaded.RoomVolume);
            Assert.Equal(123, loaded.MicrophoneGain);
            Assert.Equal(new[] { "10.1.2.3" }, loaded.RecentDestinations);
            Assert.Equal(document, await File.ReadAllTextAsync(path));
            Assert.Empty(Directory.GetFiles(directory, "*.corrupt-*"));
            await store.SaveAsync(loaded.WithRecentDestination("10.1.1.1"), CancellationToken.None);
            Assert.Contains("\"sipEngine\": \"opal\"", await File.ReadAllTextAsync(path));
        }
        finally { Directory.Delete(directory, true); }
    }

    [Fact]
    public void Ex90_is_distinct_without_changing_existing_profile_values()
    {
        Assert.Contains("Ex90", Enum.GetNames<EndpointProfile>());
        Assert.Equal(1, (int)EndpointProfile.Sx20);
        Assert.Equal(2, (int)EndpointProfile.SonyXg77);
        Assert.Equal(5, (int)EndpointProfile.SonyXg80);
        Assert.Equal(6, (int)EndpointProfile.Generic);
    }
}
