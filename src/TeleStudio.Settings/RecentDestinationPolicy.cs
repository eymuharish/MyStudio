namespace TeleStudio.Settings;
public static class RecentDestinationPolicy
{
    public static bool IsSafe(string? value)
    {
        if (string.IsNullOrWhiteSpace(value) || value.Length > 512 || value.Any(char.IsControl) || value.Any(char.IsWhiteSpace) || value.IndexOfAny(['?', '#', '=']) >= 0) return false;
        var address = value.StartsWith("sips:", StringComparison.OrdinalIgnoreCase) ? value[5..] : value.StartsWith("sip:", StringComparison.OrdinalIgnoreCase) ? value[4..] : value;
        var at = address.IndexOf('@');
        return at < 0 || !address[..at].Contains(':');
    }
}
