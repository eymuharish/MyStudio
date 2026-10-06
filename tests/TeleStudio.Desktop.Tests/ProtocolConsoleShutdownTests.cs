using System.Collections.Concurrent;
using System.Diagnostics;
using System.Text;
using TeleStudio.Contracts;

namespace TeleStudio.Desktop.Tests;

[Collection("Runtime lifecycle")]
public sealed class ProtocolConsoleShutdownTests
{
    [Fact]
    public async Task Shared_views_report_first_unconfirmed_shutdown_but_retry_retires_the_actual_stopped_worker()
    {
        var process = Create("while ($null -ne ($line = [Console]::ReadLine())) { if ($line -eq 'quit') { exit 0 } }");
        var shared = new SharedOpalConsole(process);
        await shared.Sip.StartAsync(default);
        await shared.Sip.WriteLineAsync("call synthetic", default);
        Assert.NotNull(process.ProcessId);
        await shared.Sip.DisposeAsync();
        await Assert.ThrowsAnyAsync<IOException>(() => shared.H323.DisposeAsync().AsTask());
        Assert.Null(process.ProcessId);
        await shared.Sip.DisposeAsync();
        await shared.H323.DisposeAsync();
    }
    [Fact]
    public async Task Blocked_pipe_write_has_a_deadline_and_disposal_retires_the_owned_child_safely()
    {
        var console = Create("[Console]::Out.WriteLine('PIPE-BLOCK-READY'); Start-Sleep -Seconds 40", TimeSpan.FromMilliseconds(600));
        var ready = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        console.LineReceived += (_, line) => { if (line.TryGetLegacyText(out var text) && text == "PIPE-BLOCK-READY") ready.TrySetResult(); };
        await console.StartAsync(default);
        await ready.Task.WaitAsync(TimeSpan.FromSeconds(3));
        var write = console.WriteLineAsync(new string('x', 1024 * 1024), default);
        try
        {
            await Assert.ThrowsAsync<TimeoutException>(() => write.WaitAsync(TimeSpan.FromSeconds(10)));
            Assert.True(write.IsCompleted, "The caller's external wait timed out; the production write remained unbounded.");
        }
        finally
        {
            await Assert.ThrowsAnyAsync<IOException>(() => console.DisposeAsync().AsTask().WaitAsync(TimeSpan.FromSeconds(3)));
            try { await write.WaitAsync(TimeSpan.FromSeconds(2)); } catch (Exception error) when (error is IOException or OperationCanceledException or TimeoutException) { }
        }
        await Assert.ThrowsAsync<ObjectDisposedException>(() => console.WriteLineAsync("after-retirement", default));
    }

    [Fact]
    public async Task Opal_shutdown_allows_native_terminal_after_its_sixteen_second_transaction_budget()
    {
        var lines = new ConcurrentQueue<string>();
        await using var console = Create("while ($null -ne ($line = [Console]::ReadLine())) { if ($line -eq 'quit') { Start-Sleep -Seconds 17; [Console]::Error.WriteLine('call 0: DISCONNECTED (OK)'); [Console]::Out.WriteLine('OPAL-TERMINAL-DRAINED'); exit 0 } }", OpalCallLifecycle.TerminationTimeout);
        console.LineReceived += (_, line) => { if (line.TryGetLegacyText(out var text)) lines.Enqueue(text); };
        await console.StartAsync(default);
        await console.WriteLineAsync("call synthetic", default);
        var timer = Stopwatch.StartNew();
        await console.DisposeAsync();
        Assert.Contains("OPAL-TERMINAL-DRAINED", lines);
        Assert.Contains("call 0: DISCONNECTED (OK)", lines);
        Assert.True(timer.Elapsed >= TimeSpan.FromSeconds(17));
        Assert.True(timer.Elapsed < OpalCallLifecycle.TerminationTimeout, timer.Elapsed.ToString());
    }

    [Fact]
    public async Task Configured_opal_deadline_allows_a_clean_quit_that_exceeds_the_legacy_four_seconds()
    {
        var lines = new ConcurrentQueue<string>();
        await using var console = Create("while ($null -ne ($line = [Console]::ReadLine())) { if ($line -eq 'quit') { Start-Sleep -Seconds 5; [Console]::Out.WriteLine('SLOW-GRACEFUL-QUIT'); exit 0 } }", TimeSpan.FromSeconds(8));
        console.LineReceived += (_, line) => { if (line.TryGetLegacyText(out var text)) lines.Enqueue(text); };
        await console.StartAsync(default);
        var timer = Stopwatch.StartNew();
        await console.DisposeAsync();
        Assert.Contains("SLOW-GRACEFUL-QUIT", lines);
        Assert.True(timer.Elapsed >= TimeSpan.FromSeconds(5));
        Assert.True(timer.Elapsed < TimeSpan.FromSeconds(8), timer.Elapsed.ToString());
    }

    [Fact]
    public async Task Configured_deadline_is_an_upper_bound_for_a_hung_child()
    {
        await using var console = Create("while ($null -ne ($line = [Console]::ReadLine())) { if ($line -eq 'quit') { Start-Sleep -Seconds 30 } }", TimeSpan.FromMilliseconds(600));
        await console.StartAsync(default);
        var timer = Stopwatch.StartNew();
        await Assert.ThrowsAnyAsync<IOException>(() => console.DisposeAsync().AsTask());
        Assert.True(timer.Elapsed < TimeSpan.FromSeconds(2), timer.Elapsed.ToString());
    }

    [Fact]
    public async Task Quit_keeps_both_output_pipes_draining_until_native_terminal_and_clean_exit()
    {
        var lines = new ConcurrentQueue<string>();
        await using var console = Create("""
            while ($null -ne ($line = [Console]::ReadLine())) {
                if ($line.StartsWith('call ')) { [Console]::Error.WriteLine('call 0: CONFIRMED (OK)') }
                if ($line -eq 'quit') {
                    $chunk = 'x' * 4096
                    for ($i = 0; $i -lt 256; $i++) { [Console]::Out.WriteLine($chunk); [Console]::Error.WriteLine($chunk) }
                    [Console]::Error.WriteLine('call 0: DISCONNECTED (OK)')
                    [Console]::Out.WriteLine('GRACEFUL-QUIT-COMPLETE')
                    exit 0
                }
            }
            """);
        console.LineReceived += (_, line) => { if (line.TryGetLegacyText(out var value)) lines.Enqueue(value); };
        await console.StartAsync(CancellationToken.None);
        await console.WriteLineAsync("call synthetic", CancellationToken.None);
        await console.DisposeAsync();
        Assert.Contains("GRACEFUL-QUIT-COMPLETE", lines);
        Assert.Contains("call 0: DISCONNECTED (OK)", lines);
    }

    [Fact]
    public async Task Clean_process_exit_without_call_terminal_is_not_a_confirmed_hangup()
    {
        await using var console = Create("while ($null -ne ($line = [Console]::ReadLine())) { if ($line -eq 'quit') { exit 0 } }");
        await console.StartAsync(CancellationToken.None);
        await console.WriteLineAsync("call synthetic", CancellationToken.None);
        var error = await Assert.ThrowsAnyAsync<IOException>(() => console.DisposeAsync().AsTask());
        Assert.Contains("unconfirmed", error.Message, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public async Task Hung_child_is_locally_stopped_within_five_seconds_but_not_reported_as_graceful()
    {
        await using var console = Create("while ($null -ne ($line = [Console]::ReadLine())) { if ($line -eq 'quit') { Start-Sleep -Seconds 30 } }");
        await console.StartAsync(CancellationToken.None);
        var timer = Stopwatch.StartNew();
        await Assert.ThrowsAnyAsync<IOException>(() => console.DisposeAsync().AsTask());
        Assert.True(timer.Elapsed < TimeSpan.FromSeconds(5), timer.Elapsed.ToString());
    }

    private static ProtocolConsoleProcess Create(string script, TimeSpan? shutdownTimeout = null)
    {
        var powershell = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), @"WindowsPowerShell\v1.0\powershell.exe");
        var encoded = Convert.ToBase64String(Encoding.Unicode.GetBytes(script));
        return new(CallProtocol.Sip, powershell, "-NoLogo -NoProfile -NonInteractive -EncodedCommand " + encoded, "", shutdownTimeout: shutdownTimeout);
    }

}
