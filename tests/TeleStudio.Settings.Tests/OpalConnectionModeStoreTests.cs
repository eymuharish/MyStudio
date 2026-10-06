using TeleStudio.Contracts;
using TeleStudio.Settings;

namespace TeleStudio.Settings.Tests;

public sealed class OpalConnectionModeStoreTests
{
    [Theory]
    [InlineData(NetworkConnectionMode.Building, "vpn")]
    [InlineData(NetworkConnectionMode.ExternalVpn, "vpn")]
    public async Task One_global_mode_survives_a_restart_without_creating_or_editing_profiles(NetworkConnectionMode mode, string name)
    {
        var root = Directory.CreateTempSubdirectory("OPAL global mode ");
        try
        {
            var path = Path.Combine(root.FullName, "connection-environment.json");
            var store = new OpalConnectionModeStore(path);
            Assert.Equal(NetworkConnectionMode.ExternalVpn, await store.LoadAsync(default));
            await store.SaveAsync(mode, default);
            Assert.Equal(NetworkConnectionMode.ExternalVpn, await new OpalConnectionModeStore(path).LoadAsync(default));
            Assert.Contains($"\"mode\":\"{name}\"", await File.ReadAllTextAsync(path));
            Assert.Single(Directory.GetFiles(root.FullName));
            Assert.False(Directory.Exists(Path.Combine(root.FullName, "profiles")));
            await Assert.ThrowsAsync<ArgumentOutOfRangeException>(() => store.SaveAsync((NetworkConnectionMode)999, default));
            Assert.Equal(NetworkConnectionMode.ExternalVpn, await store.LoadAsync(default));
        }
        finally { root.Delete(true); }
    }
}
