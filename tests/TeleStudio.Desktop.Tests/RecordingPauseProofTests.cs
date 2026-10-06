using MyStudio.WinUI;

namespace TeleStudio.Desktop.Tests;

public sealed class RecordingPauseProofTests
{
    [Fact]
    public void Command_and_delivery_time_extend_active_bounds_without_including_the_observed_pause()
    {
        var proof = RecordingPauseProof.Measure(400, 2700, 3500, 6600, 7000, 9000, 9600);
        Assert.Equal(4300, proof.MinimumActiveMs);
        Assert.Equal(6500, proof.MaximumActiveMs);
        Assert.Equal(3100, proof.ExcludedPauseWallMs);
        Assert.True(proof.AcceptsElapsed(6297));
        Assert.False(proof.AcceptsElapsed(9600));
        Assert.False(proof.AcceptsElapsed(2500));
    }

    [Fact]
    public void MP4_finalization_delay_is_outside_the_stop_acknowledgment_bound()
    {
        var proof = RecordingPauseProof.Measure(100, 1900, 2000, 4700, 4800, 6600, 6700);
        Assert.True(proof.AcceptsElapsed(3800));
        Assert.False(proof.AcceptsElapsed(8000));
    }

    [Fact]
    public void Out_of_order_monotonic_observations_are_rejected()
    {
        Assert.Throws<ArgumentException>(() => RecordingPauseProof.Measure(100, 1900, 1800, 4700, 4800, 6600, 6700));
    }

    [Fact]
    public async Task Fixture_cleanup_failure_is_reported_and_cannot_bypass_final_ui_shutdown()
    {
        bool shutdown = false, fixture = false;
        var errors = await SmokeCleanup.RunAsync(new Func<Task>[] {
            () => Task.FromException(new IOException("hangup failed")),
            () => { fixture = true; return Task.FromException(new IOException("fixture quit failed")); }
        }, () => { shutdown = true; return Task.CompletedTask; });
        Assert.True(fixture);
        Assert.True(shutdown);
        Assert.Equal(2, errors.Count);
    }
}
