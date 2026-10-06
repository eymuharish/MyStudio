using System.Text.Json;
using TeleStudio.Contracts;

namespace TeleStudio.Settings;

public sealed class ConnectionTuningStore(string directory)
{
    private static readonly JsonSerializerOptions Options = new() { WriteIndented = true };
    private string FilePath(EngineFamily family, CallProtocol protocol) => Path.Combine(directory, family.ToString(), $"{protocol}-connection.json");
    public async Task<ConnectionTuning> LoadAsync(EngineFamily family, CallProtocol protocol, CancellationToken token, ConnectionPreset legacyPreset = ConnectionPreset.Direct, ConnectionTuning? missingDefaults = null)
    {
        var path = FilePath(family, protocol);
        if (!File.Exists(path)) return missingDefaults ?? ConnectionTuning.Preset(legacyPreset);
        try
        {
            var settings = JsonSerializer.Deserialize<ConnectionTuning>(await File.ReadAllTextAsync(path, token), Options) ?? new();
            settings.Validate(family, protocol);
            return settings;
        }
        catch (Exception error) when (error is JsonException or ArgumentException) { return new(); }
    }
    public async Task SaveAsync(EngineFamily family, CallProtocol protocol, ConnectionTuning settings, CancellationToken token)
    {
        settings.Validate(family, protocol);
        var path = FilePath(family, protocol);
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        var temp = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try { await File.WriteAllTextAsync(temp, JsonSerializer.Serialize(settings, Options), token); File.Move(temp, path, true); }
        finally { if (File.Exists(temp)) File.Delete(temp); }
    }
}
