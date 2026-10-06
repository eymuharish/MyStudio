using System.Text.Json;
using TeleStudio.Contracts;

namespace TeleStudio.Settings;

/// <summary>The standalone OPAL edition's single connection environment, outside family profiles.</summary>
public sealed class OpalConnectionModeStore(string path)
{
    public async Task<NetworkConnectionMode> LoadAsync(CancellationToken token)
    {
        if (!File.Exists(path)) return NetworkConnectionMode.ExternalVpn;
        using var document = JsonDocument.Parse(await File.ReadAllTextAsync(path, token));
        return document.RootElement.GetProperty("mode").GetString() switch
        {
            "lan" => NetworkConnectionMode.ExternalVpn,
            "vpn" => NetworkConnectionMode.ExternalVpn,
            _ => throw new JsonException("OPAL connection mode must be lan or vpn.")
        };
    }

    public async Task SaveAsync(NetworkConnectionMode mode, CancellationToken token)
    {
        var value = mode switch
        {
            NetworkConnectionMode.Building => "vpn",
            NetworkConnectionMode.ExternalVpn => "vpn",
            _ => throw new ArgumentOutOfRangeException(nameof(mode))
        };
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path))!);
        var temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            await File.WriteAllTextAsync(temporary, JsonSerializer.Serialize(new { mode = value }), token);
            File.Move(temporary, path, true);
        }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
    }
}
