using TeleStudio.Desktop.Runtime;
namespace TeleStudio.Desktop.Tests;
public sealed class DiagnosticLogTests
{
    [Fact] public void Earlier_call_reason_survives_dense_media_diagnostics()
    {
        var dir = Directory.CreateTempSubdirectory();
        try {
            DiagnosticLog.Write("Call", "Earlier disconnect reason", dir.FullName);
            for (var i = 0; i < 1200; i++) DiagnosticLog.Write("Media", new string('x', 1500), dir.FullName);
            Assert.Contains("Earlier disconnect reason", DiagnosticLog.ReadSnapshot(dir.FullName));
        } finally { dir.Delete(true); }
    }
    [Fact] public void Snapshot_includes_operation_reason_and_redacts_quoted_and_authorization_secrets()
    {
        var dir = Directory.CreateTempSubdirectory();
        try {
            File.WriteAllText(Path.Combine(dir.FullName, "operations.log"), "Busy 486 password=\"two word secret\"\nAuthorization: Digest username=\"user\", response=\"sensitive-response\"\n");
            DiagnosticLog.Write("Call", "Remote busy", dir.FullName);
            var text = DiagnosticLog.ReadSnapshot(dir.FullName);
            Assert.Contains("Remote busy", text); Assert.Contains("Busy 486", text);
            Assert.DoesNotContain("two word secret", text); Assert.DoesNotContain("sensitive-response", text);
        } finally { dir.Delete(true); }
    }
    [Fact] public void Export_writes_readable_utf8_text_and_redacts_secrets()
    {
        var dir = Directory.CreateTempSubdirectory();
        try {
            DiagnosticLog.Write("Call", "الجهاز مشغول password=private-password", dir.FullName);
            var output = Path.Combine(dir.FullName,"export.txt");
            DiagnosticLog.ExportSnapshot(output,dir.FullName);
            Assert.Equal(new byte[] {239,187,191},File.ReadAllBytes(output).Take(3));
            Assert.Contains("الجهاز مشغول",File.ReadAllText(output));
            Assert.DoesNotContain("private-password",File.ReadAllText(output));
        } finally {dir.Delete(true);}
    }
    [Theory]
    [InlineData("https://user:private-password@host/path")]
    [InlineData("sip:user:private-password@host")]
    [InlineData("token='private-password'")]
    [InlineData("password=\"private-password with spaces\"")]
    public void Credential_forms_are_redacted(string input)
    {
        Assert.DoesNotContain("private-password", DiagnosticLog.Redact(input));
    }
    [Fact] public void Rotation_bounds_disk_usage_and_preserves_recent_events()
    {
        var dir = Directory.CreateTempSubdirectory();
        try {
            for (var i = 0; i < 900; i++) DiagnosticLog.Write("Test", new string('x', 10000), dir.FullName);
            DiagnosticLog.Write("Call", "Newest event", dir.FullName);
            Assert.All(dir.GetFiles(), f => Assert.True(f.Length <= 4 * 1024 * 1024));
            Assert.Equal(2, dir.GetFiles().Length);
            Assert.Contains("Newest event", DiagnosticLog.ReadSnapshot(dir.FullName));
        } finally { dir.Delete(true); }
    }
    [Fact] public void Inaccessible_location_does_not_throw_or_destroy_existing_file()
    {
        var dir = Directory.CreateTempSubdirectory();
        try { var path = Path.Combine(dir.FullName, "occupied"); File.WriteAllText(path,"keep");
            DiagnosticLog.Write("Call", "Failure", path);
            Assert.Contains("unavailable", DiagnosticLog.ReadSnapshot(path)); Assert.Equal("keep",File.ReadAllText(path));
        } finally {dir.Delete(true);}
    }
}


