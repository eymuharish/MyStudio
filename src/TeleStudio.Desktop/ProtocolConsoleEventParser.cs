using System.Text.RegularExpressions;
using TeleStudio.Contracts;

namespace TeleStudio.Desktop;

/// <summary>
/// Converts the stable, human-readable status lines emitted by the direct protocol
/// spikes into the application event contract. Lines without a state transition are ignored.
/// </summary>
public static partial class ProtocolConsoleEventParser
{
    public static bool TryParse(
        CallProtocol protocol,
        string? line,
        string? fallbackRemoteAddress,
        out EngineEvent? engineEvent)
    {
        engineEvent = null;
        if (string.IsNullOrWhiteSpace(line))
        {
            return false;
        }

        var text = line.Trim();
        if (text.StartsWith('{'))
        {
            return false;
        }
        return protocol == CallProtocol.H323
            ? TryParseH323(text, fallbackRemoteAddress, out engineEvent)
            : TryParseSip(text, fallbackRemoteAddress, out engineEvent);
    }

    private static bool TryParseH323(string line, string? fallbackRemoteAddress, out EngineEvent? engineEvent)
    {
        if (TryReadRemote(IncomingH323Line(), line, out var incomingRemote))
        {
            engineEvent = new IncomingCall(incomingRemote, CallProtocol.H323);
            return true;
        }

        if (TryReadRemote(EstablishedH323Line(), line, out var connectedRemote))
        {
            engineEvent = new CallConnected(connectedRemote, CallProtocol.H323, 0, 0, 0);
            return true;
        }

        var dial = DialStartH323Line().Match(line);
        if (dial.Success)
        {
            var destination = string.IsNullOrWhiteSpace(fallbackRemoteAddress)
                ? dial.Groups["destination"].Value
                : fallbackRemoteAddress;
            engineEvent = new CallProgress(destination, CallProtocol.H323, CallState.Calling);
            return true;
        }

        var cleared = ClearedH323Line().Match(line);
        if (cleared.Success)
        {
            engineEvent = new CallEnded(cleared.Groups["reason"].Value);
            return true;
        }

        var error = H323ErrorLine().Match(line);
        if (error.Success)
        {
            engineEvent = new CallFailed($"h323-{error.Groups["operation"].Value}-error", error.Groups["reason"].Value);
            return true;
        }

        engineEvent = null;
        return false;
    }

    private static bool TryParseSip(string line, string? fallbackRemoteAddress, out EngineEvent? engineEvent)
    {
        var incoming = IncomingSipLine().Match(line);
        if (incoming.Success)
        {
            engineEvent = new IncomingCall(incoming.Groups["remote"].Value, CallProtocol.Sip);
            return true;
        }

        var state = SipStateLine().Match(line);
        if (state.Success && !string.IsNullOrWhiteSpace(fallbackRemoteAddress))
        {
            var progress = state.Groups["state"].Value switch
            {
                "CALLING" => CallState.Calling,
                "EARLY" or "CONNECTING" => CallState.Negotiating,
                _ => (CallState?)null
            };
            if (progress is not null)
            {
                engineEvent = new CallProgress(fallbackRemoteAddress, CallProtocol.Sip, progress.Value);
                return true;
            }
        }

        if (state.Success && state.Groups["state"].Value == "CONFIRMED" && !string.IsNullOrWhiteSpace(fallbackRemoteAddress))
        {
            engineEvent = new CallConnected(fallbackRemoteAddress, CallProtocol.Sip, 0, 0, 0);
            return true;
        }

        if (state.Success && state.Groups["state"].Value == "DISCONNECTED")
        {
            engineEvent = new CallEnded(state.Groups["reason"].Value);
            return true;
        }

        var error = SipErrorLine().Match(line);
        if (error.Success)
        {
            engineEvent = new CallFailed("startup-error", error.Groups["detail"].Value);
            return true;
        }

        engineEvent = null;
        return false;
    }

    private static bool TryReadRemote(Regex pattern, string line, out string remote)
    {
        var match = pattern.Match(line);
        remote = match.Success ? match.Groups["remote"].Value : string.Empty;
        return match.Success;
    }

    [GeneratedRegex("^incoming-call remote=\\\"(?<remote>[^\\\"]+)\\\" action=")]
    private static partial Regex IncomingH323Line();

    [GeneratedRegex("^call-established remote=\\\"(?<remote>[^\\\"]+)\\\"$")]
    private static partial Regex EstablishedH323Line();

    [GeneratedRegex("^dial-start destination=\\\"(?<destination>[^\\\"]+)\\\" gatekeeper=disabled$")]
    private static partial Regex DialStartH323Line();

    [GeneratedRegex("^call-cleared remote=\\\"[^\\\"]+\\\" reason=(?<reason>\\S+)$")]
    private static partial Regex ClearedH323Line();

    [GeneratedRegex("^(?<operation>dial|answer|hangup)-error reason=(?<reason>\\S+)$")]
    private static partial Regex H323ErrorLine();

    [GeneratedRegex("^incoming call from (?<remote>.+); enter answer or reject$")]
    private static partial Regex IncomingSipLine();

    [GeneratedRegex("^call \\d+: (?<state>[A-Z_]+) \\((?<reason>.*)\\)$")]
    private static partial Regex SipStateLine();

    [GeneratedRegex("^startup error: (?<detail>.+)$")]
    private static partial Regex SipErrorLine();
}
