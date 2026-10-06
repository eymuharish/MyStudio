using System.Net;

namespace TeleStudio.Presentation;

public static class DestinationValidator
{
    public static bool IsValid(string? value, TeleStudio.Contracts.CallProtocol protocol)
    {
        if (IsValid(value)) return true;
        if (protocol != TeleStudio.Contracts.CallProtocol.Sip || string.IsNullOrWhiteSpace(value)) return false;
        var target = value.Trim();
        if (target.Length > 1024 || target.Any(char.IsWhiteSpace) || target.Any(char.IsControl)) return false;
        if (target.StartsWith("sips:", StringComparison.OrdinalIgnoreCase)) target = target[5..];
        else if (target.StartsWith("sip:", StringComparison.OrdinalIgnoreCase)) target = target[4..];
        var authority = target.Split(';', '?')[0]; var at = authority.LastIndexOf('@');
        if (at >= 0) { if (at == 0 || authority[..at].Contains('@')) return false; authority = authority[(at + 1)..]; }
        if (!Uri.TryCreate("http://" + authority, UriKind.Absolute, out var uri) || uri.AbsolutePath != "/" || uri.UserInfo.Length > 0 || uri.Port is < 1 or > 65535) return false;
        var host = uri.Host.Trim('[', ']');
        return IsValid(host) || IPAddress.TryParse(host, out _);
    }
    public static bool IsValid(string? value)
    {
        var destination = value?.Trim();
        if (string.IsNullOrWhiteSpace(destination) || destination.Length > 253 || destination.Contains(' '))
        {
            return false;
        }

        if (IPAddress.TryParse(destination, out var address))
        {
            return address.AddressFamily == System.Net.Sockets.AddressFamily.InterNetwork;
        }

        if (destination.All(character => char.IsDigit(character) || character == '.'))
        {
            return false;
        }

        return Uri.CheckHostName(destination) == UriHostNameType.Dns;
    }
}
