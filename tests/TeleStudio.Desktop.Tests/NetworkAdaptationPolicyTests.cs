using TeleStudio.Contracts;
using TeleStudio.Desktop;

namespace TeleStudio.Desktop.Tests;

public sealed class NetworkAdaptationPolicyTests
{
    [Theory]
    [InlineData(1, false)]
    [InlineData(3, false)]
    [InlineData(5, true)]
    public void Five_sample_loss_window_has_a_strict_three_percent_threshold(double loss, bool reduce)
    {
        var controller = new NetworkAdaptationController(1000, 15);
        for (var second = 0; second < 4; second++) Assert.Null(controller.Observe(second, loss, 0));
        var change = controller.Observe(4, loss, 0);
        if (reduce) Assert.Equal(800, change!.BitrateKbps);
        else Assert.Null(change);
    }

    [Fact]
    public void Queue_only_pressure_reduces_but_queue_only_recovery_is_not_inferred()
    {
        var controller = new NetworkAdaptationController(1000, 15);
        for (var second = 0; second < 4; second++) Assert.Null(controller.Observe(second, null, 200));
        Assert.Equal(800, controller.Observe(4, null, 200)!.BitrateKbps);
        for (var second = 5; second < 100; second++) Assert.Null(controller.Observe(second, null, 0));
    }

    [Fact]
    public void Text_preserves_resolution_until_pressure_continues_at_two_fps()
    {
        var controller = new NetworkAdaptationController(1000, 5, ScreenContentMode.Text, VideoResolution.Fhd);
        var changes = Collect(controller, 0, 24, 5);
        Assert.Equal(3, changes.Count);
        Assert.Equal(2, changes[0].Video.FramesPerSecond);
        Assert.Null(changes[0].Video.Resolution);
        Assert.Equal((14, VideoResolution.Hd), (changes[1].At, changes[1].Video.Resolution));
        Assert.Equal((24, VideoResolution.Sd), (changes[2].At, changes[2].Video.Resolution));
    }

    [Fact]
    public void Motion_preserves_frame_rate_by_reducing_resolution_first()
    {
        var controller = new NetworkAdaptationController(1000, 30, ScreenContentMode.Motion, VideoResolution.Fhd);
        var changes = Collect(controller, 0, 24, 5);
        Assert.Equal((30, VideoResolution.Hd), (changes[0].Video.FramesPerSecond, changes[0].Video.Resolution));
        Assert.Equal((30, VideoResolution.Sd), (changes[1].Video.FramesPerSecond, changes[1].Video.Resolution));
        Assert.Equal(24, changes[2].Video.FramesPerSecond);
        Assert.Null(changes[2].Video.Resolution);
    }

    [Fact]
    public void Camera_uses_motion_priority_and_never_drops_below_five_fps()
    {
        var controller = new NetworkAdaptationController(1000, 30, ScreenContentMode.Text, VideoResolution.Hd, desktop: false);
        var changes = Collect(controller, 0, 200, 5);
        Assert.Equal(VideoResolution.Sd, changes[0].Video.Resolution);
        Assert.Equal(30, changes[0].Video.FramesPerSecond);
        Assert.All(changes, c => Assert.InRange(c.Video.FramesPerSecond, 5, 30));
        Assert.Equal(5, changes[^1].Video.FramesPerSecond);
    }

    [Fact]
    public void Unknown_auto_resolution_is_never_invented()
    {
        var controller = new NetworkAdaptationController(1000, 30, ScreenContentMode.Motion, VideoResolution.Auto);
        Assert.All(Collect(controller, 0, 300, 5), c => Assert.Null(c.Video.Resolution));
    }

    [Fact]
    public void Alternating_bad_and_good_periods_do_not_oscillate_or_violate_cooldown()
    {
        var controller = new NetworkAdaptationController(1000, 15);
        var previousBitrate = 1000;
        var previousChange = -10;
        var count = 0;
        for (var second = 0; second < 180; second++)
        {
            var change = controller.Observe(second, second / 5 % 2 == 0 ? 5 : 0, 0);
            if (change is null) continue;
            Assert.True(second - previousChange >= 10);
            Assert.InRange(change.BitrateKbps, 128, previousBitrate);
            previousBitrate = change.BitrateKbps;
            previousChange = second;
            count++;
        }
        Assert.True(count > 2);
    }

    [Fact]
    public void Recovery_is_gradual_requires_thirty_healthy_seconds_and_respects_ceiling()
    {
        var controller = new NetworkAdaptationController(1000, 15, ScreenContentMode.Text, VideoResolution.Hd);
        var changes = Collect(controller, 0, 44, 5);
        var previous = changes[^1].Video;
        var restoredResolution = VideoResolution.Sd;
        for (var second = 45; second <= 500; second++)
        {
            var change = controller.Observe(second, .5, 0);
            if (change is null) continue;
            Assert.True(second >= 75);
            Assert.InRange(change.BitrateKbps, previous.BitrateKbps, Math.Min(1000, (int)Math.Ceiling(previous.BitrateKbps * 1.1)));
            Assert.InRange(change.FramesPerSecond, previous.FramesPerSecond, 15);
            if (change.Resolution is { } resolution) restoredResolution = resolution;
            previous = change;
        }
        Assert.Equal(1000, previous.BitrateKbps);
        Assert.Equal(15, previous.FramesPerSecond);
        Assert.Equal(VideoResolution.Hd, restoredResolution);
    }

    [Fact]
    public void Lowest_allowed_ceiling_never_increases_during_degradation()
    {
        var controller = new NetworkAdaptationController(128, 2);
        var changes = Collect(controller, 0, 150, 5);
        Assert.Equal(2, changes.Count);
        Assert.All(changes, c => Assert.Equal(128, c.Video.BitrateKbps));
        Assert.Equal(VideoResolution.Sd, changes[^1].Video.Resolution);
    }

    [Fact]
    public void Duplicate_and_invalid_samples_cannot_form_a_measurement_window()
    {
        var controller = new NetworkAdaptationController(1000, 15);
        for (var repeat = 0; repeat < 100; repeat++) Assert.Null(controller.Observe(0, 5, 200));
        Assert.Null(controller.Observe(double.NaN, 5, 200));
        Assert.Null(controller.Observe(1, -1, 200));
        Assert.Null(controller.Observe(2, 5, double.PositiveInfinity));
        for (var second = 3; second < 7; second++) Assert.Null(controller.Observe(second, 5, 0));
        Assert.Equal(800, controller.Observe(7, 5, 0)!.BitrateKbps);
    }

    [Fact]
    public void Missing_sample_gap_restarts_the_healthy_observation_period()
    {
        var controller = new NetworkAdaptationController(1000, 15);
        for (var second = 0; second <= 4; second++) controller.Observe(second, 5, 0);
        for (var second = 5; second <= 24; second++) controller.Observe(second, 0, 0);
        for (var second = 100; second <= 129; second++) Assert.Null(controller.Observe(second, 0, 0));
    }

    [Fact]
    public void Unknown_measurements_do_not_count_as_healthy_audio_transport()
    {
        var controller = new NetworkAdaptationController(1000, 15);
        for (var second = 0; second <= 4; second++) controller.Observe(second, 5, 0);
        for (var second = 5; second <= 24; second++) controller.Observe(second, 0, 0);
        Assert.Null(controller.Observe(25, null, null));
        for (var second = 26; second <= 50; second++) Assert.Null(controller.Observe(second, 0, 0));
    }

    private static List<(int At, AdaptedVideo Video)> Collect(NetworkAdaptationController controller, int start, int end, double loss)
    {
        var result = new List<(int, AdaptedVideo)>();
        for (var second = start; second <= end; second++)
            if (controller.Observe(second, loss, 0) is { } change) result.Add((second, change));
        return result;
    }
}
