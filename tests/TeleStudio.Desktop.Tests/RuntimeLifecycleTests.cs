using System.Diagnostics;
using TeleStudio.Desktop.Runtime;

namespace TeleStudio.Desktop.Tests;

[CollectionDefinition("Runtime lifecycle", DisableParallelization = true)]
public sealed class RuntimeLifecycleCollection;

[Collection("Runtime lifecycle")]
public sealed class RuntimeLifecycleTests
{
    [Fact]
    public void Reused_pid_or_different_executable_never_matches_owned_identity()
    {
        var expected = new RuntimeProcessIdentity(123, 456, @"C:\Apps\EssaStudio\opal_console.exe");
        Assert.True(RuntimeProcessIdentity.Matches(expected, expected with { ExecutablePath = @"c:\apps\ESSASTUDIO\opal_console.exe" }));
        Assert.False(RuntimeProcessIdentity.Matches(expected, expected with { StartUtcTicks = 457 }));
        Assert.False(RuntimeProcessIdentity.Matches(expected, expected with { ProcessId = 124 }));
        Assert.False(RuntimeProcessIdentity.Matches(expected, expected with { ExecutablePath = @"C:\Other\opal_console.exe" }));
    }

    [Fact]
    public void Same_version_in_another_path_is_a_takeover_not_window_activation()
    {
        var identity = new RuntimeProcessIdentity(123, 456, @"C:\Apps\EssaStudio\EssaStudio.exe");
        var record = new RuntimeSessionRecord(identity, "1.20.0.0", new string('a', 64), []);
        Assert.True(RuntimeSessionBroker.SameApplication(record, identity with { ProcessId = 124 }, "1.20.0.0"));
        Assert.False(RuntimeSessionBroker.SameApplication(record, identity, "1.21.0.0"));
        Assert.False(RuntimeSessionBroker.SameApplication(record,
            identity with { ExecutablePath = @"C:\Other\EssaStudio.exe" }, "1.20.0.0"));
    }

    [Fact]
    public void Current_process_identity_can_be_verified_but_changed_start_cannot()
    {
        using var current = Process.GetCurrentProcess();
        var identity = RuntimeProcessIdentity.Capture(current);
        using var verified = identity.OpenVerified();
        Assert.NotNull(verified);
        Assert.Null((identity with { StartUtcTicks = identity.StartUtcTicks - 1 }).OpenVerified());
    }

    [Fact]
    public async Task Second_instance_activates_primary_and_cannot_take_its_lease()
    {
        var directory = Path.Combine(Path.GetTempPath(), "EssaStudio-lifecycle-test-" + Guid.NewGuid().ToString("N"));
        try
        {
            var activated = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            using var primary = new RuntimeSessionBroker(directory, path => Directory.CreateDirectory(path));
            primary.ActivateRequested = () => activated.TrySetResult();
            Assert.True(await primary.AcquireAsync());
            using var duplicate = new RuntimeSessionBroker(directory, path => Directory.CreateDirectory(path));
            Assert.False(await duplicate.AcquireAsync());
            await activated.Task.WaitAsync(TimeSpan.FromSeconds(2));
            Assert.True(primary.IsPrimary);
            Assert.False(duplicate.IsPrimary);
        }
        finally { if (Directory.Exists(directory)) Directory.Delete(directory, recursive: true); }
    }

    [Fact]
    public async Task Job_disposal_stops_only_the_explicitly_enrolled_child()
    {
        if (!OperatingSystem.IsWindows()) return;
        using var child = StartWaitingProcess();
        using var unrelated = StartWaitingProcess();
        IReadOnlyList<RuntimeProcessIdentity> identities = [];
        OwnedEngineProcesses.Changed = values => identities = values;
        try
        {
            OwnedEngineProcesses.Register(child);
            Assert.Single(identities);
            Assert.Equal(child.Id, identities[0].ProcessId);
            OwnedEngineProcesses.TerminateChildren();
            await child.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(3));
            Assert.True(child.HasExited);
            Assert.False(unrelated.HasExited);
            Assert.Empty(identities);
        }
        finally
        {
            OwnedEngineProcesses.TerminateChildren();
            OwnedEngineProcesses.Changed = null;
            foreach (var process in new[] { child, unrelated })
                if (!process.HasExited) process.Kill(entireProcessTree: true);
        }
    }

    [Fact]
    public async Task Exit_deadline_includes_a_cleanup_task_that_never_finishes()
    {
        var never = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var coordinator = new ApplicationShutdownCoordinator(TimeSpan.FromMilliseconds(30));
        var elapsed = Stopwatch.StartNew();
        try
        {
            var accepted = await coordinator.ExecuteAsync(true, true,
                _ => never.Task, () => new ValueTask(never.Task));
            Assert.True(accepted);
            Assert.True(coordinator.FinalCloseAllowed);
            Assert.InRange(elapsed.Elapsed.TotalSeconds, 4.5, 6.0);
        }
        finally { never.TrySetResult(); }
    }

    [Fact]
    public async Task Quarantined_forged_legacy_identity_cannot_terminate_an_unrelated_process()
    {
        var directory = Directory.CreateTempSubdirectory("EssaStudio-runtime-forged-test-");
        using var unrelated = StartWaitingProcess();
        try
        {
            var identity = RuntimeProcessIdentity.Capture(unrelated);
            var forged = new RuntimeSessionRecord(identity, "1.20.0.0", new string('a', 64), [identity]);
            File.WriteAllText(Path.Combine(directory.FullName, "session.json"), System.Text.Json.JsonSerializer.Serialize(forged));
            RuntimeDirectorySecurity.QuarantineLegacyFiles(directory.FullName);
            using var broker = new RuntimeSessionBroker(directory.FullName, path => Directory.CreateDirectory(path));
            Assert.True(await broker.AcquireAsync());
            Assert.False(unrelated.HasExited);
        }
        finally
        {
            if (!unrelated.HasExited) unrelated.Kill(entireProcessTree: true);
            directory.Delete(true);
        }
    }

    [Fact]
    public void Fresh_child_identity_is_available_before_waiting_for_loader_initialization()
    {
        for (var index = 0; index < 5; index++)
        {
            using var child = StartWaitingProcess();
            try
            {
                var identity = RuntimeProcessIdentity.Capture(child);
                Assert.Equal(child.Id, identity.ProcessId);
                Assert.Equal(child.StartTime.ToUniversalTime().Ticks, identity.StartUtcTicks);
                Assert.Equal(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "WindowsPowerShell", "v1.0", "powershell.exe"), identity.ExecutablePath, ignoreCase: true);
            }
            finally { if (!child.HasExited) child.Kill(entireProcessTree: true); }
        }
    }

    private static Process StartWaitingProcess() => Process.Start(new ProcessStartInfo
    {
        FileName = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "WindowsPowerShell", "v1.0", "powershell.exe"),
        Arguments = "-NoProfile -NonInteractive -Command Start-Sleep -Seconds 30",
        UseShellExecute = false,
        CreateNoWindow = true
    })!;
}
