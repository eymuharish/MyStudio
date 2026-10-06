using TeleStudio.Contracts;
using TeleStudio.Settings;

namespace TeleStudio.Settings.Tests;

public sealed class VideoSettingsTests : IDisposable
{
    private readonly string _directory = Path.Combine(Path.GetTempPath(), $"TeleStudio.VideoSettings.Tests.{Guid.NewGuid():N}");

    [Fact]
    public async Task Video_preferences_round_trip_with_exact_tokens_and_survive_recent_destination()
    {
        var store = new SettingsStore(Path.Combine(_directory, "settings.json"));
        var settings = new AppSettings
        {
            VideoResolution = VideoResolution.Fhd,
            VideoBitrateKbps = 128,
            CameraOverlaySize = CameraOverlaySize.Small,
            CameraOverlayPosition = CameraOverlayPosition.TopLeft,
            CameraId = "camera-1",
        };
        await store.SaveAsync(settings, CancellationToken.None);
        var json = await File.ReadAllTextAsync(Path.Combine(_directory, "settings.json"));
        Assert.Contains("\"videoResolution\": \"fhd\"", json);
        Assert.Contains("\"cameraOverlayPosition\": \"top-left\"", json);
        var loaded = await store.LoadAsync(CancellationToken.None);
        Assert.Equal(settings.WithRecentDestination("10.0.0.2"), loaded.WithRecentDestination("10.0.0.2"));
    }

    [Fact]
    public async Task Invalid_video_bitrate_preserves_corrupt_file_behavior()
    {
        Directory.CreateDirectory(_directory);
        var path = Path.Combine(_directory, "settings.json");
        await File.WriteAllTextAsync(path, "{\"language\":\"ar\",\"rtpPortStart\":40000,\"rtpPortEnd\":40199,\"recentDestinations\":[],\"videoBitrateKbps\":127}");
        Assert.Equal(AppSettings.Default, await new SettingsStore(path).LoadAsync(CancellationToken.None));
        Assert.Single(Directory.GetFiles(_directory, "settings.corrupt-*.json"));
    }

    public void Dispose()
    {
        if (Directory.Exists(_directory)) Directory.Delete(_directory, recursive: true);
    }
}
