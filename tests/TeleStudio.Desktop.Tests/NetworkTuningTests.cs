using TeleStudio.Contracts;
using TeleStudio.Desktop;

namespace TeleStudio.Desktop.Tests;

public sealed class NetworkTuningTests
{
    [Fact]
    public void Total_budget_reserves_audio_and_overhead_without_raising_video_ceiling()
    {
        var value = new ConnectionTuning { TotalSendCeilingKbps = 1024, VideoCeilingKbps = 3000 };
        Assert.Equal(633, value.EffectiveVideoCeilingKbps);
        Assert.Contains(" 633 ", value.VideoCommand(true));
        Assert.Equal(300, (value with { VideoCeilingKbps = 300 }).EffectiveVideoCeilingKbps);
    }
    [Fact]
    public void External_profile_bounds_jitter_and_preserves_audio_priority()
    {
        var profile = ConnectionTuning.Preset(ConnectionPreset.External);
        Assert.Equal(100, profile.JitterMinimumMs);
        Assert.Equal(600, profile.JitterMaximumMs);
        Assert.Equal(100, profile.SendBufferMs);
        Assert.Equal(1500, profile.VideoCeilingKbps);
    }

    [Fact]
    public void Adaptation_waits_for_window_and_cooldown_then_reduces_twenty_percent()
    {
        var controller = new NetworkAdaptationController(1000, 15);
        for (var second = 0; second < 4; second++) Assert.Null(controller.Observe(second, 5, 0));
        var change = controller.Observe(4, 5, 0);
        Assert.NotNull(change);
        Assert.Equal(800, change!.BitrateKbps);
        for (var second = 5; second < 14; second++) Assert.Null(controller.Observe(second, 5, 0));
        Assert.Equal(640, controller.Observe(14, 5, 0)!.BitrateKbps);
    }

    [Fact]
    public void Explicit_wrong_vendor_codec_is_rejected()
    {
        Assert.Throws<ArgumentException>(() => (ConnectionTuning.Preset(ConnectionPreset.Direct) with
        { AudioCodec = AudioSelection.AacLd }).Validate(EngineFamily.Sony, CallProtocol.H323));
    }
}
