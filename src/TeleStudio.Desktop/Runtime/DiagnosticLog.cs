using System.IO;
using System.Text;
using System.Text.RegularExpressions;

namespace TeleStudio.Desktop.Runtime;

/// <summary>Local, bounded application diagnostics. Never throws into the call lifecycle.</summary>
public static class DiagnosticLog
{
    // Two bounded files retain call transitions across a teaching day; periodic
    // meter sampling in WinUI prevents media counters displacing call reasons.
    private const int MaxBytes = 4 * 1024 * 1024;
    private static readonly object Gate = new();
    public static string DirectoryPath => Path.Combine(TeleStudio.Settings.AppDataPaths.Root, "logs");

    public static void Write(string category, string message) => Write(category, message, DirectoryPath);
    internal static void Write(string category, string message, string directory)
    {
        try
        {
            lock (Gate)
            {
                EnsureSafe(directory);
                Directory.CreateDirectory(directory);
                var path = Path.Combine(directory, "events.log");
                var previous = Path.Combine(directory, "events.previous.log");
                EnsureSafeFile(path); EnsureSafeFile(previous);
                var entry = $"{DateTimeOffset.Now:O} [{Redact(category[..Math.Min(category.Length, 100)])}] {Redact(message[..Math.Min(message.Length, 12000)])}{Environment.NewLine}";
                if (File.Exists(path) && new FileInfo(path).Length + Encoding.UTF8.GetByteCount(entry) > MaxBytes)
                    File.Move(path, previous, true);
                File.AppendAllText(path, entry, Encoding.UTF8);
            }
        }
        catch { /* Logging must not interrupt calls or shutdown. */ }
    }

    public static void ExportSnapshot(string path) => ExportSnapshot(path, DirectoryPath);
    internal static void ExportSnapshot(string path, string directory)
        => File.WriteAllText(path, ReadSnapshot(directory), new UTF8Encoding(true));

    public static string ReadSnapshot(int maxBytesPerFile = MaxBytes) => ReadSnapshot(DirectoryPath, maxBytesPerFile);
    internal static string ReadSnapshot(string directory, int maxBytesPerFile = MaxBytes)
    {
        try
        {
            lock (Gate)
            {
                EnsureSafe(directory);
                maxBytesPerFile = Math.Clamp(maxBytesPerFile, 1024, MaxBytes);
                var output = new StringBuilder("EssaStudio diagnostics / سجل التشخيص\n");
                output.AppendLine($"Collected: {DateTimeOffset.Now:O}");
                output.AppendLine($"Version: {typeof(DiagnosticLog).Assembly.GetName().Version}; OS: {Environment.OSVersion}; Runtime: {Environment.Version}");
                var found = false;
                foreach (var name in new[] { "startup.log", "operations.log", "events.previous.log", "events.log" })
                {
                    var path = Path.Combine(directory, name);
                    EnsureSafeFile(path);
                    if (!File.Exists(path)) continue;
                    found = true;
                    output.AppendLine($"\n--- {name} (recent entries) ---");
                    using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
                    if (stream.Length > maxBytesPerFile) stream.Seek(-maxBytesPerFile, SeekOrigin.End);
                    using var reader = new StreamReader(stream, Encoding.UTF8, true);
                    if (stream.Position > 0) reader.ReadLine(); // Drop partial leading UTF-8 line.
                    output.AppendLine(Redact(reader.ReadToEnd()));
                }
                if (!found) output.AppendLine("لا توجد أحداث مسجلة بعد / No diagnostic events recorded yet.");
                return output.ToString();
            }
        }
        catch { return "تعذر قراءة سجل التشخيص / Diagnostic log unavailable."; }
    }

    internal static string Redact(string text)
    {
        try
        {
            var timeout = TimeSpan.FromMilliseconds(100);
            text = Regex.Replace(text, @"(?im)\b(?:proxy-)?authorization\s*[:=][^\r\n]*", "Authorization: [redacted]", RegexOptions.None, timeout);
            text = Regex.Replace(text, "(?i)(\\b(?:password|passwd|pwd|token|secret|api[-_]?key|credential)\\s*[\\\"']?\\s*[:=]\\s*)(?:\\\"[^\\\"]*\\\"|'[^']*'|[^\\s;,]+)", "$1[redacted]", RegexOptions.None, timeout);
            text = Regex.Replace(text, @"((?:https?://|sips?:))[^/@\s]+@", "$1[redacted]@", RegexOptions.IgnoreCase, timeout);
            return text;
        }
        catch (RegexMatchTimeoutException) { return "[diagnostic entry omitted: redaction timeout]"; }
    }
    private static void EnsureSafe(string directory)
    {
        if (File.Exists(directory)) throw new IOException("Log directory is a file.");
        for (var item = new DirectoryInfo(directory); item is not null; item = item.Parent)
            if (item.Exists && (item.Attributes & FileAttributes.ReparsePoint) != 0) throw new IOException("Redirected log directory.");
    }
    private static void EnsureSafeFile(string path)
    {
        if (File.Exists(path) && (File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0) throw new IOException("Redirected log file.");
    }
}


