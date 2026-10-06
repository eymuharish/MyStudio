using System.Diagnostics;
using System.Text;
using TeleStudio.Contracts;

namespace TeleStudio.Desktop.Tests;

[Collection("Runtime lifecycle")]
public sealed class NativeStartupTests
{
    [Theory]
    [InlineData("startup error: call-setup-failed: No media")]
    [InlineData("dial-error reason=call-setup-failed")]
    public async Task Shared_worker_call_failure_confirms_call_termination_for_clean_shutdown(string failure)
    {
        var executable = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), @"WindowsPowerShell\v1.0\powershell.exe");
        var script = "[Console]::Error.WriteLine('Listening SIP test'); while (($line = [Console]::ReadLine()) -ne 'quit') { if ($line.StartsWith('call ')) { [Console]::Error.WriteLine('" + failure + "') } }";
        var console = new ProtocolConsoleProcess(CallProtocol.Sip, executable,
            "-NoLogo -NoProfile -NonInteractive -EncodedCommand " + Convert.ToBase64String(Encoding.Unicode.GetBytes(script)), "Listening SIP", sharedProtocols: true);
        var failed = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        console.LineReceived += (_, line) => { if (line.TryGetLegacyText(out var text) && text == failure) failed.TrySetResult(); };
        try
        {
            await console.StartAsync(default);
            await console.WriteLineAsync("call 127.0.0.1", default);
            await failed.Task.WaitAsync(TimeSpan.FromSeconds(5));
        }
        finally { await console.DisposeAsync(); }
    }

    [Fact]
    public async Task Opal_startup_returns_only_after_both_listeners_are_ready()
    {
        var executable = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), @"WindowsPowerShell\v1.0\powershell.exe");
        var script = "[Console]::Error.WriteLine('Listening SIP on 5140'); Start-Sleep -Milliseconds 700; [Console]::Error.WriteLine('Listening H.323 on 1760'); while ([Console]::ReadLine() -ne 'quit') {}";
        await using var console = new ProtocolConsoleProcess(CallProtocol.Sip, executable,
            "-NoLogo -NoProfile -NonInteractive -EncodedCommand " + Convert.ToBase64String(Encoding.Unicode.GetBytes(script)),
            "Listening SIP on ", additionalReadyPrefixes: ["Listening H.323 on "], sharedProtocols: true);
        var h323Ready = false;
        console.LineReceived += (_, line) => { if (line.TryGetLegacyText(out var text) && text.StartsWith("Listening H.323 on ")) h323Ready = true; };
        await console.StartAsync(default);
        Assert.True(h323Ready, "OPAL process returned after SIP readiness before H.323 was initialized.");
    }

    private static ProtocolConsoleProcess Create(string script)
    {
        var executable = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), @"WindowsPowerShell\v1.0\powershell.exe");
        return new(CallProtocol.Sip, executable,
            "-NoLogo -NoProfile -NonInteractive -EncodedCommand " + Convert.ToBase64String(Encoding.Unicode.GetBytes(script)), "Listening SIP");
    }

    [Fact]
    public async Task Startup_waits_for_listener_ready_instead_of_assuming_process_creation_is_ready()
    {
        await using var console = Create("Start-Sleep -Milliseconds 700; [Console]::Error.WriteLine('Listening SIP test'); while ([Console]::ReadLine() -ne 'quit') {}");
        var ready = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        console.LineReceived += (_, line) => { if (line.TryGetLegacyText(out var text) && text.StartsWith("Listening SIP")) ready.TrySetResult(); };
        await console.StartAsync(CancellationToken.None);
        Assert.True(ready.Task.IsCompleted, "Process returned before listener initialization completed");
    }

    [Fact]
    public async Task Delayed_startup_failure_reports_actual_executable_and_exit_code()
    {
        var console = Create("Start-Sleep -Milliseconds 400; exit 37");
        try
        {
            var error = await Assert.ThrowsAsync<InvalidOperationException>(() => console.StartAsync(CancellationToken.None));
            Assert.Contains("powershell.exe", error.Message);
            Assert.Contains("0x00000025", error.Message);
        }
        finally { try { await console.DisposeAsync(); } catch (IOException) { } }
    }

    [Fact]
    public async Task Cancelled_initialization_stops_the_owned_child()
    {
        var console = Create("Start-Sleep -Seconds 30");
        using var deadline = new CancellationTokenSource(TimeSpan.FromMilliseconds(500));
        try { await Assert.ThrowsAnyAsync<OperationCanceledException>(() => console.StartAsync(deadline.Token)); }
        finally { try { await console.DisposeAsync(); } catch (IOException) { } }
    }

}
