using System.Net;
using System.Text.Json;
using System.Text.Json.Serialization;
using TeleStudio.Contracts;
using TeleStudio.Settings;

namespace TeleStudio.Desktop;

public sealed record OpalStudioAudioMode(string RequestedCodec, string NegotiatedSend, string NegotiatedReceive, bool Stable,
    string QualityNoteAr = "", string QualityNoteEn = "", int Channels = 1, int Kbps = 64);
public sealed record OpalStudioProtocol(CallProtocol Protocol, OpalStudioAudioMode[] AudioModes);
public sealed record OpalSupportedStudio(string Id, string Address, string[] Names, EndpointProfile EndpointProfile,
    CallProtocol PreferredProtocol, string NotesAr, string NotesEn, OpalStudioProtocol[] Protocols)
{
    public EngineFamily Family => EndpointProfile is EndpointProfile.SonyXg77 or EndpointProfile.SonyXg80 ? EngineFamily.Sony : EngineFamily.Cisco;
}

/// <summary>Observed protocol support controls dialing. AAC preference may fall back
/// to an observed G.722 path; SDP chooses the actual codec and channel count.</summary>
public sealed record OpalStudioCatalog(int SchemaVersion, OpalSupportedStudio[] Studios)
{
    public static OpalStudioCatalog Parse(string json)
    {
        var options = new JsonSerializerOptions { PropertyNameCaseInsensitive = true };
        options.Converters.Add(new JsonStringEnumConverter());
        var catalog = JsonSerializer.Deserialize<OpalStudioCatalog>(json, options) ?? throw new JsonException("Studio support catalog is empty.");
        if (catalog.SchemaVersion != 1 || catalog.Studios is null) throw new JsonException("Unsupported studio support catalog schema.");
        var addresses = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var studio in catalog.Studios)
        {
            if (string.IsNullOrWhiteSpace(studio.Id) || !IPAddress.TryParse(studio.Address, out var address) ||
                address.AddressFamily != System.Net.Sockets.AddressFamily.InterNetwork || !addresses.Add(studio.Address) ||
                studio.EndpointProfile is not (EndpointProfile.Sx20 or EndpointProfile.Ex90 or EndpointProfile.SonyXg77 or EndpointProfile.SonyXg80) ||
                !Enum.IsDefined(studio.PreferredProtocol) || studio.Names is null || studio.Protocols is null)
                throw new JsonException("Invalid studio support identity.");
            foreach (var name in studio.Names)
                if (string.IsNullOrWhiteSpace(name) || !names.Add(name.Trim())) throw new JsonException("Ambiguous studio support name.");
            if (studio.Protocols.Select(x => x.Protocol).Distinct().Count() != studio.Protocols.Length) throw new JsonException("Duplicate studio protocol.");
            foreach (var protocol in studio.Protocols)
                if (!Enum.IsDefined(protocol.Protocol) || protocol.AudioModes is null || protocol.AudioModes.Any(mode =>
                    mode.RequestedCodec is not ("g722" or "aac-lc" or "aac-ld") ||
                    mode.NegotiatedSend is not ("g722" or "aac-lc" or "aac-ld") || mode.NegotiatedReceive is not ("g722" or "aac-lc" or "aac-ld") ||
                    mode.Channels is < 1 or > 2 || mode.Kbps <= 0))
                    throw new JsonException("Invalid studio codec evidence.");
        }
        return catalog;
    }

    public OpalSupportedStudio? Find(string? destination)
    {
        var value = destination?.Trim() ?? "";
        var named = Studios.FirstOrDefault(x => x.Names.Contains(value, StringComparer.OrdinalIgnoreCase));
        if (named is not null) return named;
        if (value.StartsWith("#sip:@", StringComparison.OrdinalIgnoreCase)) value = value[6..];
        else if (value.StartsWith("#h323:@", StringComparison.OrdinalIgnoreCase)) value = value[7..];
        else if (value.StartsWith("sip:", StringComparison.OrdinalIgnoreCase)) value = value[4..];
        else if (value.StartsWith("sips:", StringComparison.OrdinalIgnoreCase)) value = value[5..];
        else if (value.StartsWith("h323:", StringComparison.OrdinalIgnoreCase)) value = value[5..];
        if (value.LastIndexOf('@') is var at && at >= 0) value = value[(at + 1)..];
        value = value.Split(':', ';', '?')[0];
        return Studios.FirstOrDefault(x => x.Address == value || x.Names.Contains(value, StringComparer.OrdinalIgnoreCase));
    }

    public bool Allows(string? destination, CallProtocol protocol, bool aacEnabled) => Find(destination) is not { } studio || Allows(studio, protocol, aacEnabled);
    public bool Allows(OpalSupportedStudio studio, CallProtocol protocol, bool aacEnabled)
    {
        var requested = aacEnabled ? studio.Family == EngineFamily.Sony ? "aac-lc" : "aac-ld" : "g722";
        return studio.Protocols.Where(x => x.Protocol == protocol).SelectMany(x => x.AudioModes).Any(mode =>
            (mode.RequestedCodec == requested || aacEnabled && mode.RequestedCodec == "g722") && ModeVerified(studio, protocol, mode));
    }
    private static bool ModeVerified(OpalSupportedStudio studio, CallProtocol protocol, OpalStudioAudioMode mode)
    {
        var aac = studio.Family == EngineFamily.Sony ? "aac-lc" : "aac-ld";
        var codec = studio.Family == EngineFamily.Sony ? AudioSelection.AacLc : AudioSelection.AacLd;
        return mode.Stable && (mode.RequestedCodec == "g722"
            ? mode.NegotiatedSend == "g722" && mode.NegotiatedReceive == "g722" && mode.Channels == 1 && mode.Kbps == 64
            : mode.RequestedCodec == aac && (mode.NegotiatedSend == aac || mode.NegotiatedReceive == aac) &&
                AudioFormatOptions.For(codec, protocol).Contains(new AudioFormatOption(mode.Channels, mode.Kbps)));
    }
    public AudioFormatOption[] AllowedFormats(string? destination, CallProtocol protocol, EngineFamily family, bool aacEnabled)
    {
        var codec = aacEnabled ? family == EngineFamily.Sony ? AudioSelection.AacLc : AudioSelection.AacLd : AudioSelection.G722;
        if (Find(destination) is not { } studio) return AudioFormatOptions.For(codec, protocol);
        if (aacEnabled) return Allows(studio, protocol, true) ? AudioFormatOptions.For(codec, protocol) : [];
        var requested = aacEnabled ? studio.Family == EngineFamily.Sony ? "aac-lc" : "aac-ld" : "g722";
        return studio.Protocols.Where(p => p.Protocol == protocol).SelectMany(p => p.AudioModes)
            .Where(m => m.RequestedCodec == requested && ModeVerified(studio, protocol, m))
            .Select(m => new AudioFormatOption(m.Channels, m.Kbps)).Distinct().ToArray();
    }
    public CallProtocol[] AllowedProtocols(string? destination) => Find(destination) is not { } studio
        ? Enum.GetValues<CallProtocol>() : Enum.GetValues<CallProtocol>().Where(p => Allows(studio, p, false) || Allows(studio, p, true)).ToArray();
    public IReadOnlyList<RoomDirectoryEntry> ApplyDirectory(IReadOnlyList<RoomDirectoryEntry> rooms, bool addMissing = true)
    {
        var result = rooms.Select(room => Find(room.Destination) is { } studio ? room with { EndpointProfile = studio.EndpointProfile } : room).ToList();
        foreach (var studio in Studios.Where(studio => addMissing && !result.Any(room => Find(room.Destination)?.Id == studio.Id)))
            result.Add(new("opal-" + studio.Id, studio.Id.All(char.IsDigit) ? "Studio" + studio.Id : studio.Names[0], studio.Address, studio.EndpointProfile));
        return result;
    }
    public (CallProtocol Protocol, bool AacEnabled)? Recommendation(OpalSupportedStudio studio)
    {
        var protocols = new[] { studio.PreferredProtocol }.Concat(Enum.GetValues<CallProtocol>()).Distinct();
        foreach (var aac in new[] { true, false }) foreach (var protocol in protocols)
            if (Allows(studio, protocol, aac)) return (protocol, aac);
        return null;
    }
    public OpalSupportedStudio? RequireAllowed(string destination, CallProtocol protocol, bool aacEnabled, int? channels = null, int? kbps = null)
    {
        var studio = Find(destination);
        if (studio is not null && (!Allows(studio, protocol, aacEnabled) || channels is not null && kbps is not null &&
            !AllowedFormats(destination, protocol, studio.Family, aacEnabled).Contains(new AudioFormatOption(channels.Value, kbps.Value))))
            throw new InvalidOperationException("This studio protocol/codec has no verified working connection. Choose a supported option.");
        return studio;
    }
    public string Notes(string? destination, bool arabic)
    {
        var studio = Find(destination);
        if (studio is null) return arabic ? "عنوان غير مختبر؛ استخدم إعدادات الاتصال اليدوية." : "Untested address; use manual connection settings.";
        if (Recommendation(studio) is null) return arabic ? "لم يتم اعتماد اتصال لهذا الاستديو؛ يلزم إعادة الاختبار" : "No connection is approved for this studio; retesting is required.";
        var pairs = studio.Protocols.SelectMany(p => p.AudioModes.Where(m => ModeVerified(studio, p.Protocol, m))
            .Select(m => p.Protocol + " · TX " + m.NegotiatedSend.ToUpperInvariant() + " / RX " + m.NegotiatedReceive.ToUpperInvariant())).Distinct();
        var quality = studio.Protocols.SelectMany(p => p.AudioModes).Where(m => m.Stable)
            .Select(m => arabic ? m.QualityNoteAr : m.QualityNoteEn).Where(x => !string.IsNullOrWhiteSpace(x)).Distinct();
        return string.Join(" · ", new[] { arabic ? studio.NotesAr : studio.NotesEn }.Concat(pairs).Concat(quality).Where(x => !string.IsNullOrWhiteSpace(x)));
    }
}
