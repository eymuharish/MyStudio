using MyStudio.WinUI;
namespace TeleStudio.Desktop.Tests;

public class CallBarPolicyTests
{
    [Theory]
    [InlineData(true, true, "System starting", false)]
    [InlineData(false, false, "System stopping", true)]
    public void Pending_disabled_audio_button_reports_confirmation_wait_and_preserves_actual_capture_state(bool off, bool requested, string label, bool active)
    {
        var state = CallToolState.Create("System", "Stop system audio", "Share system audio", off, false, requested: requested);
        Assert.Equal(label, state.Label); Assert.Equal(active, state.Active);
        Assert.DoesNotContain("unavailable", state.Description, StringComparison.OrdinalIgnoreCase);
    }
    // Catch timer coupling, negative elapsed values and the 24-hour TimeSpan wrap.
    [Theory]
    [InlineData(65, 12000, "00:01:05", "00:00:12")]
    [InlineData(93784, 0, "26:03:04", "00:00:00")]
    [InlineData(-4, -1000, "00:00:00", "00:00:00")]
    public void Call_and_recording_displays_use_independent_elapsed_values(long callSeconds, long recordingMs, string call, string recording)
    {
        var result = CallBarTimers.Create(TimeSpan.FromSeconds(callSeconds), recordingMs);
        Assert.Equal(call, result.Call);
        Assert.Equal(recording, result.Recording);
    }
    [Fact]
    public void Confirmed_mute_toggles_have_distinct_state_and_action()
    {
        for (var i = 0; i < 12; i++)
        {
            var muted = i % 2 == 1;
            var state = CallToolState.Create("Mic", "Mute microphone", "Unmute microphone", muted, true);
            Assert.Equal(muted, state.CrossedOut);
            Assert.Equal(muted ? "Mic off" : "Mic on", state.Label);
            Assert.Contains(muted ? "Unmute microphone" : "Mute microphone", state.Description);
        }
    }
    [Fact]
    public void Unavailable_state_does_not_claim_microphone_is_transmitting()
    {
        var state = CallToolState.Create("Mic", "Mute microphone", "Unmute microphone", false, false);
        Assert.Equal("Mic unavailable", state.Label);
        Assert.Contains("Unavailable", state.Description);
        Assert.False(state.Active);
    }
    [Theory]
    [InlineData(-2000, -100, -1920, 0)]
    [InlineData(500, 1500, -460, 920)]
    [InlineData(-1200, 400, -1200, 400)]
    public void Drag_stays_inside_negative_origin_monitor(int x, int y, int expectedX, int expectedY)
    {
        var bounds = CallBarBounds.Clamp(x, y, 460, 160, -1920, 0, 1920, 1080);
        Assert.Equal(expectedX, bounds.X); Assert.Equal(expectedY, bounds.Y);
    }
    [Fact]
    public void Small_work_area_and_dpi_size_changes_remain_visible()
    {
        var bounds = CallBarBounds.Clamp(500, 500, 690, 240, 40, 20, 600, 200);
        Assert.Equal(new CallBarBounds(40, 20, 600, 200), bounds);
    }
    [Fact]
    public void Grip_requires_motion_threshold_before_dragging()
    {
        Assert.False(CallBarBounds.ShouldDrag(3, 3));
        Assert.True(CallBarBounds.ShouldDrag(4, 0));
        Assert.True(CallBarBounds.ShouldDrag(0, -5));
    }
    [Fact]
    public void Hiding_persists_across_refreshes_and_reopen_and_new_call_restore_bar()
    {
        var visibility = new CallBarVisibility();
        Assert.False(visibility.ShouldShow(false)); Assert.True(visibility.ShouldShow(true));
        visibility.Hide(); for (var i = 0; i < 100; i++) Assert.False(visibility.ShouldShow(true));
        visibility.Reopen(); Assert.True(visibility.ShouldShow(true));
        visibility.Hide(); Assert.False(visibility.ShouldShow(false)); Assert.True(visibility.ShouldShow(true));
    }
    [Theory]
    [InlineData(true, true, "Camera starting", "Stop camera")]
    [InlineData(false, false, "Camera stopping", "Start camera")]
    public void Pending_media_preserves_actual_action_and_does_not_claim_requested_media_confirmed(bool off, bool requested, string label, string action)
    {
        var state = CallToolState.Create("Camera", "Stop camera", "Start camera", off, true, requested: requested);
        Assert.Equal(label, state.Label); Assert.Contains(action, state.Description); Assert.Equal(!off, state.Active);
    }
}
