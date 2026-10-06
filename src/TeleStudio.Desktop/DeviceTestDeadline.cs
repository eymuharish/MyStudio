using System.Net;
namespace TeleStudio.Desktop;

internal sealed record DeviceTestDeadline(DateTimeOffset Cutoff)
{
    public static DeviceTestDeadline? FromEnvironment()
    {
        var text = Environment.GetEnvironmentVariable("ESSASTUDIO_DEVICE_TEST_CUTOFF_UTC");
        if (string.IsNullOrWhiteSpace(text)) return null;
        if (!DateTimeOffset.TryParse(text, System.Globalization.CultureInfo.InvariantCulture, System.Globalization.DateTimeStyles.AssumeUniversal, out var time))
            throw new ArgumentException("ESSASTUDIO_DEVICE_TEST_CUTOFF_UTC must be a UTC timestamp.");
        return new(time.ToUniversalTime());
    }
    public bool BlockStart(string? destination, DateTimeOffset now) => !IsLocal(destination) && now >= Cutoff.AddSeconds(-45);
    public bool EndCall(string? destination, DateTimeOffset now) => !IsLocal(destination) && now >= Cutoff.AddSeconds(-45);
    private static bool IsLocal(string? destination)
    {
        if (string.IsNullOrWhiteSpace(destination)) return false;
        var host = destination.Trim().Trim('<', '>');
        if (host.StartsWith("ip$", StringComparison.OrdinalIgnoreCase)) host = host[3..];
        foreach (var prefix in new[] { "sips:", "sip:", "h323:" })
            if (host.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)) { host = host[prefix.Length..]; break; }
        var at = host.LastIndexOf('@'); if (at >= 0) host = host[(at + 1)..];
        host = host.Split(';', '?', '/')[0];
        if (host.StartsWith('[')) { var end = host.IndexOf(']'); host = end > 0 ? host[1..end] : host; }
        else if (host.Count(c => c == ':') == 1) host = host.Split(':')[0];
        return host.Equals("localhost", StringComparison.OrdinalIgnoreCase) || IPAddress.TryParse(host, out var address) && IPAddress.IsLoopback(address);
    }
}
