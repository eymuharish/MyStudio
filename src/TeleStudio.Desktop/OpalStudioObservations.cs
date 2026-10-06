using System.IO;
using System.Net;
using System.Text.Json;
using System.Text.RegularExpressions;
using TeleStudio.Contracts;

namespace TeleStudio.Desktop;

public sealed record OpalStudioObservation(string Address, DateTimeOffset UpdatedAt, string State, CallProtocol Protocol,
    AudioCodecKind? AudioCodec = null, long? DecodedAudioSamples = null, long? DeviceSubmittedNonzeroSamples = null,
    long? DecodedVideoFrames = null, string? FailureCode = null);

/// <summary>Last observed call facts. These never add or approve protocol/codec support.</summary>
public sealed class OpalStudioObservations
{
    private readonly Dictionary<string, OpalStudioObservation> records = new(StringComparer.Ordinal);
    private readonly string? path;
    private string? activeAddress;
    public OpalStudioObservations(string? path = null)
    {
        this.path = path;
        try
        {
            if (path is null || !File.Exists(path) || new FileInfo(path).Length > 262144) return;
            foreach (var item in JsonSerializer.Deserialize<OpalStudioObservation[]>(File.ReadAllText(path)) ?? [])
                if (records.Count < 64 && Normalize(item.Address) == item.Address && Enum.IsDefined(item.Protocol) &&
                    item.State is "dialing" or "connected" or "ended" or "failed") records[item.Address] = item;
        }
        catch { /* A missing or damaged observation file cannot block a call. */ }
    }
    public OpalStudioObservation[] Snapshot() => records.Values.OrderByDescending(x => x.UpdatedAt).ToArray();
    public OpalStudioObservation? Find(string address) => Normalize(address) is { } key && records.TryGetValue(key, out var result) ? result : null;
    public void Begin(string address, CallProtocol protocol)
    {
        activeAddress = Normalize(address);
        if (activeAddress is null) return;
        records[activeAddress] = new(activeAddress, DateTimeOffset.UtcNow, "dialing", protocol);
        Trim(); Save();
    }
    public void Apply(EngineEvent value)
    {
        if (activeAddress is null || !records.TryGetValue(activeAddress, out var current)) return;
        var terminal = false;
        switch (value)
        {
            case CallConnected connected when Normalize(connected.RemoteAddress) == activeAddress && connected.Protocol == current.Protocol:
                current = current with { State = "connected" }; break;
            case AudioCodecStateChanged audio:
                current = current with { AudioCodec = audio.Negotiated }; break;
            case SipConnectionHealthChanged health:
                current = current with { DecodedAudioSamples = Math.Max(0, health.DecodedAudioSamples),
                    DeviceSubmittedNonzeroSamples = Math.Max(0, health.DeviceSubmittedNonzeroSamples),
                    DecodedVideoFrames = Math.Max(0, health.ReceivedFrames) }; break;
            case CallEnded: current = current with { State = "ended" }; terminal = true; break;
            case CallFailed failed:
                var code = Regex.IsMatch(failed.ErrorCode, "^[a-zA-Z0-9._-]{1,80}$") ? failed.ErrorCode : "call-failed";
                current = current with { State = "failed", FailureCode = code }; terminal = true; break;
            default: return;
        }
        records[activeAddress] = current with { UpdatedAt = DateTimeOffset.UtcNow };
        if (value is not SipConnectionHealthChanged) Save();
        if (terminal) activeAddress = null;
    }
    private void Trim()
    {
        foreach (var old in records.Values.OrderByDescending(x => x.UpdatedAt).Skip(64).ToArray()) records.Remove(old.Address);
    }
    private void Save()
    {
        if (path is null) return;
        var temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllText(temporary, JsonSerializer.Serialize(Snapshot()));
            File.Move(temporary, path, true);
        }
        catch (Exception error) { Runtime.DiagnosticLog.Write("studio-observations", error.GetType().Name); }
        finally { try { if (File.Exists(temporary)) File.Delete(temporary); } catch { } }
    }
    private static string? Normalize(string destination)
    {
        var value = destination.Trim();
        if (value.LastIndexOf('@') is var at && at >= 0) value = value[(at + 1)..];
        else foreach (var prefix in new[] { "sip:", "sips:", "h323:" })
            if (value.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)) { value = value[prefix.Length..]; break; }
        value = value.Split(':', ';', '?')[0];
        return IPAddress.TryParse(value, out var address) && address.AddressFamily == System.Net.Sockets.AddressFamily.InterNetwork ? address.ToString() : null;
    }
}
