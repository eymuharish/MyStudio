using System.Reflection;

namespace TeleStudio.Desktop;

public static class AppRelease
{
    public static string DisplayVersion { get; } = typeof(AppRelease).Assembly
        .GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion
        ?? $"v{typeof(AppRelease).Assembly.GetName().Version}";
}
