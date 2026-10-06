namespace TeleStudio.Desktop;

/// <summary>Opt-in physical GUI verification is limited to the authorized target matrix.</summary>
public sealed record StudioTransitionSmokePlan(int Calls, string SonyAddress)
{
    public const string CiscoAddress = "10.136.51.25";
    public static StudioTransitionSmokePlan Create(string? calls, string? sonyAddress)
    {
        var count = calls is null ? 4 : int.TryParse(calls, out var parsed) ? parsed : 0;
        if (count is not (4 or 8)) throw new ArgumentException("Studio transition smoke accepts exactly 4 or 8 calls.");
        var address = sonyAddress ?? "10.136.51.11";
        if (address is not ("10.136.51.11" or "10.136.61.31"))
            throw new ArgumentException("The Sony target must belong to the authorized transition matrix.");
        return new(count, address);
    }
    public static string RequireIsolation(string? specifiedRoot, string defaultRoot)
    {
        if (string.IsNullOrWhiteSpace(specifiedRoot)) throw new ArgumentException("Studio transition smoke requires explicit --data-root.");
        var root = Path.GetFullPath(specifiedRoot);
        if (string.Equals(Path.TrimEndingDirectorySeparator(root), Path.TrimEndingDirectorySeparator(Path.GetFullPath(defaultRoot)), StringComparison.OrdinalIgnoreCase))
            throw new ArgumentException("Studio transition smoke cannot use the application's normal data root.");
        return root;
    }
}
