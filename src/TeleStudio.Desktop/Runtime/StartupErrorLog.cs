using System.IO;
using System.Text;
using System.Text.RegularExpressions;

namespace TeleStudio.Desktop.Runtime;

internal static class StartupErrorLog
{
    internal static bool TryWriteOperation(Exception error, string operation, out string logPath, string? logDirectory = null)
        => TryWriteCore(error, out logPath, logDirectory, "operations.log", operation);

    internal static bool TryWrite(Exception error, out string logPath, string? logDirectory = null)
        => TryWriteCore(error, out logPath, logDirectory, "startup.log", "Startup");

    private static bool TryWriteCore(Exception error, out string logPath, string? logDirectory, string fileName, string operation)
    {
        logPath = string.Empty;
        try
        {
            logDirectory ??= Path.Combine(TeleStudio.Settings.AppDataPaths.Root, "logs");
            logPath = Path.Combine(logDirectory, fileName);
            for (var item = new DirectoryInfo(logDirectory); item is not null; item = item.Parent)
                if (item.Exists && (item.Attributes & FileAttributes.ReparsePoint) != 0) return false;
            if (File.Exists(logPath) && (File.GetAttributes(logPath) & FileAttributes.ReparsePoint) != 0) return false;
            var output = new StringBuilder().AppendLine(DateTimeOffset.UtcNow.ToString("O"));
            output.AppendLine("Operation: " + Redact(operation));
            for (var current = error; current is not null && output.Length < 32768; current = current.InnerException)
            {
                output.AppendLine(current.GetType().FullName);
                output.AppendLine(Redact(current.Message));
                output.AppendLine(Redact(current.StackTrace ?? "(stack unavailable)"));
            }
            Directory.CreateDirectory(logDirectory);
            File.AppendAllText(logPath, output.ToString() + Environment.NewLine, Encoding.UTF8);
            return true;
        }
        catch (Exception) { return false; } // Diagnostics must never hide the original startup failure.
    }

    private static string Redact(string text)
    {
        text = Regex.Replace(text, @"(?i)(authorization\s*[:=]\s*)(?:Bearer\s+|Basic\s+)?[^\s;,]+", "$1[redacted]", RegexOptions.None, TimeSpan.FromMilliseconds(100));
        text = Regex.Replace(text, "(?i)((?:password|passwd|pwd|token|secret|api[-_]?key|credential)\\s*[\\\"']?\\s*[:=]\\s*[\\\"']?)[^\\s;,\\\"']+", "$1[redacted]", RegexOptions.None, TimeSpan.FromMilliseconds(100));
        return Regex.Replace(text, @"(https?://)[^/@\s]+@", "$1[redacted]@", RegexOptions.IgnoreCase, TimeSpan.FromMilliseconds(100));
    }
}
