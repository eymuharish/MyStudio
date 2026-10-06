namespace TeleStudio.Settings;

/// <summary>Process-scoped data override for portable evaluation and recovery children.</summary>
public static class AppDataPaths
{
    public const string EnvironmentVariable = "ESSASTUDIO_DATA_ROOT";
    public static string? OverrideRoot => Environment.GetEnvironmentVariable(EnvironmentVariable) is { Length: > 0 } value ? ResolveRoot(value) : null;
    public static string Root => OverrideRoot ?? ResolveRoot(null);
    public static string ResolveRoot(string? value)
    {
        if (string.IsNullOrWhiteSpace(value)) return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "EssaStudio");
        if (!Path.IsPathFullyQualified(value)) throw new ArgumentException("Data root must be an absolute path.");
        return Path.GetFullPath(value);
    }
    public static (string[] Remaining, string? Root) ParseArguments(string[] args)
    {
        var remaining = new List<string>();
        string? root = null;
        for (var index = 0; index < args.Length; index++)
        {
            if (!string.Equals(args[index], "--data-root", StringComparison.OrdinalIgnoreCase)) { remaining.Add(args[index]); continue; }
            if (root is not null || ++index >= args.Length || string.IsNullOrWhiteSpace(args[index]))
                throw new ArgumentException("Specify --data-root once followed by an absolute directory.");
            root = ResolveRoot(args[index]);
        }
        return (remaining.ToArray(), root);
    }
}
