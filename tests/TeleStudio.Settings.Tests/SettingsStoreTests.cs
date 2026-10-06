using System.Text;
using System.Text.Json;
using TeleStudio.Contracts;
using TeleStudio.Settings;

namespace TeleStudio.Settings.Tests;

public sealed class SettingsStoreTests : IDisposable
{
    private readonly string _directory = Path.Combine(Path.GetTempPath(), $"TeleStudio.Settings.Tests.{Guid.NewGuid():N}");

    [Fact]
    public async Task Managed_media_settings_round_trip_with_exact_lowercase_tokens()
    {
        var path = Path.Combine(_directory, "settings.json");
        var store = new SettingsStore(path);
        var expected = new AppSettings
        {
            Language = "en",
            Codec = CodecPreference.H264,
            Quality = QualityMode.Maximum,
            Endpoint = EndpointProfile.SonyXg77,
            LecturerName = "Essa Studio",
            AudioDscp = 32,
            VideoDscp = 34,
            SignalingDscp = 24,
            RoomVolume = 75,
            MicrophoneGain = 125,
            NoiseIsolation = false,
            NetworkMode = NetworkConnectionMode.ExternalVpn,
        };

        await store.SaveAsync(expected, CancellationToken.None);
        var json = await File.ReadAllTextAsync(path);
        var actual = await store.LoadAsync(CancellationToken.None);

        Assert.Equal(expected, actual);
        Assert.Contains("\"codec\": \"h264\"", json);
        Assert.Contains("\"quality\": \"maximum\"", json);
        Assert.Contains("\"endpoint\": \"pcs-xg77\"", json);
        Assert.Contains("\"networkMode\": \"external-vpn\"", json);
        Assert.DoesNotContain("recording", json, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public async Task Old_settings_receive_media_defaults_but_invalid_or_numeric_enums_are_preserved_as_corrupt()
    {
        Directory.CreateDirectory(_directory);
        var path = Path.Combine(_directory, "settings.json");
        await File.WriteAllTextAsync(path, """
            {"language":"ar","rtpPortStart":40000,"rtpPortEnd":40199,"recentDestinations":[]}
            """);
        var store = new SettingsStore(path);

        var migrated = await store.LoadAsync(CancellationToken.None);

        Assert.Equal(CodecPreference.Auto, migrated.Codec);
        Assert.Equal(QualityMode.Auto, migrated.Quality);
        Assert.Equal(EndpointProfile.Auto, migrated.Endpoint);
        Assert.Equal("EssaStudio", migrated.LecturerName);
        Assert.Equal((32, 34, 24), (migrated.AudioDscp, migrated.VideoDscp, migrated.SignalingDscp));
        Assert.Equal((100, 100, true), (migrated.RoomVolume, migrated.MicrophoneGain, migrated.NoiseIsolation));

        await File.WriteAllTextAsync(path, """
            {"language":"ar","rtpPortStart":40000,"rtpPortEnd":40199,"recentDestinations":[],"codec":1}
            """);
        Assert.Equal(AppSettings.Default, await store.LoadAsync(CancellationToken.None));
        Assert.Single(Directory.GetFiles(_directory, "settings.corrupt-*.json"));
    }

    [Fact]
    public async Task Null_collection_and_invalid_lecturer_or_ranges_use_corrupt_recovery()
    {
        Directory.CreateDirectory(_directory);
        foreach (var invalid in new[]
                 {
                     "{\"language\":\"ar\",\"recentDestinations\":null}",
                     "{\"language\":\"ar\",\"recentDestinations\":[],\"lecturerName\":\"عيسى\"}",
                     "{\"language\":\"ar\",\"recentDestinations\":[],\"audioDscp\":64}",
                     "{\"language\":\"ar\",\"recentDestinations\":[],\"roomVolume\":101}",
                     "{\"language\":\"ar\",\"recentDestinations\":[],\"microphoneGain\":151}"
                 })
        {
            var path = Path.Combine(_directory, $"settings-{Guid.NewGuid():N}.json");
            await File.WriteAllTextAsync(path, invalid);
            Assert.Equal(AppSettings.Default, await new SettingsStore(path).LoadAsync(CancellationToken.None));
            Assert.False(File.Exists(path));
        }
    }

    [Fact]
    public async Task Save_then_load_preserves_language_devices_and_rtp_range()
    {
        var store = new SettingsStore(Path.Combine(_directory, "settings.json"));
        var expected = new AppSettings
        {
            Language = "en",
            CameraId = "camera-1",
            MicrophoneId = "microphone-1",
            SpeakerId = "speaker-1",
            RtpPortStart = 42000,
            RtpPortEnd = 42199,
        };

        await store.SaveAsync(expected, CancellationToken.None);
        var actual = await store.LoadAsync(CancellationToken.None);

        Assert.Equal(expected, actual);
    }

    [Fact]
    public void Add_recent_destination_keeps_ten_unique_newest_first()
    {
        var settings = AppSettings.Default;
        for (var index = 1; index <= 11; index++)
        {
            settings = settings.WithRecentDestination($"10.0.0.{index}");
        }
        settings = settings.WithRecentDestination("10.0.0.5");

        Assert.Equal(10, settings.RecentDestinations.Count);
        Assert.Equal("10.0.0.5", settings.RecentDestinations[0]);
        Assert.DoesNotContain("10.0.0.1", settings.RecentDestinations);
        Assert.Equal(1, settings.RecentDestinations.Count(item => item == "10.0.0.5"));
    }

    [Fact]
    public async Task Corrupt_file_returns_defaults_and_preserves_evidence_copy()
    {
        Directory.CreateDirectory(_directory);
        var path = Path.Combine(_directory, "settings.json");
        await File.WriteAllTextAsync(path, "{broken", Encoding.UTF8);
        var store = new SettingsStore(path);

        var actual = await store.LoadAsync(CancellationToken.None);

        Assert.Equal(AppSettings.Default, actual);
        Assert.False(File.Exists(path));
        Assert.Single(Directory.GetFiles(_directory, "settings.corrupt-*.json"));
    }

    [Fact]
    public async Task Missing_corrupt_or_language_missing_settings_default_English_but_saved_Arabic_survives()
    {
        Directory.CreateDirectory(_directory); var path = Path.Combine(_directory, "language-default.json"); var store = new SettingsStore(path);
        Assert.Equal("en", (await store.LoadAsync(CancellationToken.None)).Language);
        await File.WriteAllTextAsync(path, "{broken"); Assert.Equal("en", (await store.LoadAsync(CancellationToken.None)).Language);
        await File.WriteAllTextAsync(path, "{}"); Assert.Equal("en", (await store.LoadAsync(CancellationToken.None)).Language);
        await store.SaveAsync(new AppSettings { Language = "ar" }, CancellationToken.None);
        Assert.Equal("ar", (await store.LoadAsync(CancellationToken.None)).Language);
    }

    public void Dispose()
    {
        if (Directory.Exists(_directory))
        {
            Directory.Delete(_directory, recursive: true);
        }
    }
}
