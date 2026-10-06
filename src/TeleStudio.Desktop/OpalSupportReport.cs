using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using TeleStudio.Contracts;

namespace TeleStudio.Desktop;

public static class OpalSupportReport
{
    public static string Build(object safeSnapshot, IEnumerable<EngineEvent> events,
        IEnumerable<OpalStudioObservation> observations, string persistedLog, string buildIdentity)
    {
        var options = new JsonSerializerOptions { WriteIndented = true };
        var text = new StringBuilder("MyStudio support log / سجل الدعم الشامل\n");
        text.AppendLine($"Collected: {DateTimeOffset.Now:O}");
        text.AppendLine($"OS: {RuntimeInformation.OSDescription}; OS architecture: {RuntimeInformation.OSArchitecture}; process architecture: {RuntimeInformation.ProcessArchitecture}; runtime: {RuntimeInformation.FrameworkDescription}");
        text.AppendLine("\nBuild identity\n" + buildIdentity);
        text.AppendLine("\nConnection and media snapshot\n" + JsonSerializer.Serialize(safeSnapshot, options));
        text.AppendLine("\nLatest studio observations (not protocol certification)\n" + JsonSerializer.Serialize(observations, options));
        text.AppendLine("\nRecent engine events");
        foreach (var item in events.TakeLast(1000)) text.AppendLine(item.GetType().Name + " " + JsonSerializer.Serialize(item, item.GetType()));
        text.AppendLine("\nPersistent diagnostics\n" + persistedLog);
        text.AppendLine("\nRemote pixels and human-heard audio require separate verification. No media files or credentials are included.");
        return Runtime.DiagnosticLog.Redact(text.ToString());
    }
}
