using TeleStudio.Contracts;
using TeleStudio.Settings;

namespace TeleStudio.Settings.Tests;

public sealed class OpalPreferenceMigrationTests
{
    [Theory]
    [InlineData("\"building\"")]
    [InlineData("0")]
    public async Task Legacy_network_preferences_load_as_vpn_without_losing_or_rewriting_user_data(string mode)
    {
        var folder = Directory.CreateTempSubdirectory("OPAL preference migration ");
        try
        {
            var path = Path.Combine(folder.FullName, "settings.json");
            var document = "{\"networkMode\":" + mode + ",\"lecturerName\":\"Saved Name\",\"roomVolume\":43,\"recentDestinations\":[\"10.0.0.20\"],\"opalVideoDefaultsVersion\":1}";
            await File.WriteAllTextAsync(path, document);
            var store = new SettingsStore(path); var loaded = await store.LoadAsync(default);
            Assert.Equal(NetworkConnectionMode.ExternalVpn, loaded.NetworkMode);
            Assert.Equal("Saved Name", loaded.LecturerName); Assert.Equal(43, loaded.RoomVolume);
            Assert.Equal(new[] { "10.0.0.20" }, loaded.RecentDestinations); Assert.Equal(1, loaded.OpalVideoDefaultsVersion);
            Assert.Equal(document, await File.ReadAllTextAsync(path));
            Assert.Single(Directory.GetFiles(folder.FullName));
            await store.SaveAsync(loaded, default);
            Assert.Contains("\"networkMode\": \"external-vpn\"", await File.ReadAllTextAsync(path));
            Assert.Equal(loaded, await store.LoadAsync(default));
        }
        finally { folder.Delete(true); }
    }

    [Fact]
    public async Task Old_global_lan_mode_migrates_without_rewriting_original_document()
    {
        var folder = Directory.CreateTempSubdirectory("OPAL global migration ");
        try
        {
            var path = Path.Combine(folder.FullName, "connection-environment.json");
            const string document = "{\"mode\":\"lan\"}"; await File.WriteAllTextAsync(path, document);
            Assert.Equal(NetworkConnectionMode.ExternalVpn, await new OpalConnectionModeStore(path).LoadAsync(default));
            Assert.Equal(document, await File.ReadAllTextAsync(path));
        }
        finally { folder.Delete(true); }
    }

    [Fact]
    public void Video_migration_preserves_marker_and_other_preferences_through_recent_history_and_device_edits()
    {
        var original = new AppSettings { LecturerName = "Name", RoomVolume = 42, MicrophoneId = "mic", RecentDestinations = ["a"], VideoResolution = VideoResolution.Hd, VideoBitrateKbps = 2000 };
        var migrated = AppSettingsCopy.CopyVideo(original, VideoResolution.Fhd, 4000, 1);
        Assert.Equal(VideoResolution.Fhd, migrated.VideoResolution); Assert.Equal(4000, migrated.VideoBitrateKbps);
        Assert.Equal(1, migrated.OpalVideoDefaultsVersion); Assert.Equal("Name", migrated.LecturerName);
        Assert.Equal(42, migrated.RoomVolume); Assert.Equal("mic", migrated.MicrophoneId); Assert.Equal(new[] { "a" }, migrated.RecentDestinations);
        Assert.Equal(original.Codec, migrated.Codec); Assert.Equal(original.Quality, migrated.Quality);
        Assert.Equal(1, migrated.WithRecentDestination("b").OpalVideoDefaultsVersion);
        Assert.Equal(1, AppSettingsCopy.Copy(migrated, "Updated", "ar").OpalVideoDefaultsVersion);
        Assert.NotEqual(original, migrated); Assert.Equal(VideoResolution.Hd, original.VideoResolution);
    }
}
