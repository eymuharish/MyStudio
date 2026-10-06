namespace TeleStudio.Desktop.Tests;

public class ApplicationShutdownCoordinatorTests
{
    [Fact]
    public async Task Confirmed_exit_hangs_up_before_cleanup_and_allows_final_close()
    {
        var calls = new List<string>();
        var coordinator = new ApplicationShutdownCoordinator(TimeSpan.FromSeconds(1));

        var accepted = await coordinator.ExecuteAsync(
            confirmed: true,
            hasActiveCall: true,
            _ => { calls.Add("hangup"); return Task.CompletedTask; },
            () => { calls.Add("cleanup"); return ValueTask.CompletedTask; });

        Assert.True(accepted);
        Assert.True(coordinator.FinalCloseAllowed);
        Assert.Equal(["hangup", "cleanup"], calls);
    }

    [Fact]
    public async Task Rejected_exit_does_not_disconnect_or_cleanup()
    {
        var calls = new List<string>();
        var coordinator = new ApplicationShutdownCoordinator(TimeSpan.FromSeconds(1));

        var accepted = await coordinator.ExecuteAsync(
            confirmed: false,
            hasActiveCall: true,
            _ => { calls.Add("hangup"); return Task.CompletedTask; },
            () => { calls.Add("cleanup"); return ValueTask.CompletedTask; });

        Assert.False(accepted);
        Assert.False(coordinator.FinalCloseAllowed);
        Assert.Empty(calls);
    }

    [Fact]
    public async Task Cleanup_still_runs_when_hangup_fails()
    {
        var calls = new List<string>();
        var coordinator = new ApplicationShutdownCoordinator(TimeSpan.FromMilliseconds(50));

        var accepted = await coordinator.ExecuteAsync(
            confirmed: true,
            hasActiveCall: true,
            _ => throw new IOException("engine unavailable"),
            () => { calls.Add("cleanup"); return ValueTask.CompletedTask; });

        Assert.True(accepted);
        Assert.True(coordinator.FinalCloseAllowed);
        Assert.Equal(["cleanup"], calls);
    }
}
