using TeleStudio.Desktop.Runtime;

namespace TeleStudio.Desktop.Tests;

public sealed class StartupErrorLogTests
{
    [Fact]
    public void Runtime_failure_has_separate_operation_log_with_context_and_original_reason()
    {
        var directory = Directory.CreateTempSubdirectory("EssaStudio-operation-log-test-");
        try
        {
            Assert.True(StartupErrorLog.TryWriteOperation(new IOException("Native pipe closed"), "HangUp/H323", out var path, directory.FullName));
            Assert.Equal(Path.Combine(directory.FullName, "operations.log"), path);
            var text = File.ReadAllText(path);
            Assert.Contains("HangUp/H323", text);
            Assert.Contains("Native pipe closed", text);
            Assert.Contains("IOException", text);
            Assert.False(File.Exists(Path.Combine(directory.FullName, "startup.log")));
        }
        finally { directory.Delete(true); }
    }

    [Fact]
    public void Startup_failure_records_type_reason_stack_and_appends_without_destroying_prior_evidence()
    {
        var directory = Directory.CreateTempSubdirectory("EssaStudio-startup-log-test-");
        try
        {
            Exception failure;
            try { throw new UnauthorizedAccessException("Runtime directory rejected"); }
            catch (Exception error) { failure = error; }
            Assert.True(StartupErrorLog.TryWrite(failure, out var path, directory.FullName));
            Assert.Equal(Path.Combine(directory.FullName, "startup.log"), path);
            var first = File.ReadAllText(path);
            Assert.Contains("UnauthorizedAccessException", first);
            Assert.Contains("Runtime directory rejected", first);
            Assert.Contains(nameof(Startup_failure_records_type_reason_stack_and_appends_without_destroying_prior_evidence), first);
            Assert.True(StartupErrorLog.TryWrite(new IOException("Second failure"), out _, directory.FullName));
            Assert.StartsWith(first, File.ReadAllText(path));
        }
        finally { directory.Delete(true); }
    }

    [Fact]
    public void Unwritable_log_location_never_replaces_original_startup_failure_or_existing_file()
    {
        var directory = Directory.CreateTempSubdirectory("EssaStudio-startup-log-test-");
        try
        {
            var occupied = Path.Combine(directory.FullName, "occupied");
            File.WriteAllText(occupied, "Preserved");
            Assert.False(StartupErrorLog.TryWrite(new IOException("Original failure"), out _, occupied));
            Assert.Equal("Preserved", File.ReadAllText(occupied));
        }
        finally { directory.Delete(true); }
    }

    [Fact]
    public void Startup_diagnostics_redact_credential_values_and_do_not_serialize_exception_data()
    {
        var directory = Directory.CreateTempSubdirectory("EssaStudio-startup-log-test-");
        try
        {
            var failure = new IOException("password=private-password; token=private-token; Authorization: Bearer private-bearer https://user:private-uri@host/path");
            failure.Data["private"] = "private-data";
            Assert.True(StartupErrorLog.TryWrite(failure, out var path, directory.FullName));
            var text = File.ReadAllText(path);
            foreach (var secret in new[] { "private-password", "private-token", "private-bearer", "private-uri", "private-data" })
                Assert.DoesNotContain(secret, text);
            Assert.Contains("IOException", text);
        }
        finally { directory.Delete(true); }
    }
}
