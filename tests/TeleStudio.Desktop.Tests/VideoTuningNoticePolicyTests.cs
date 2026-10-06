using TeleStudio.Contracts;
namespace TeleStudio.Desktop.Tests;
public class VideoTuningNoticePolicyTests
{
    [Theory]
    [InlineData("video-preferences-failed", "Encoder rejected bitrate change")]
    [InlineData("network-video-rejected", "Encoder rejected network profile")]
    [InlineData("network-video-not-ready", "Video negotiation is not yet ready.")]
    public void Only_live_rejections_can_stay_in_the_log(string code, string detail)
    {
        var error = new MediaError(code, detail);
        Assert.True(VideoTuningNoticePolicy.IsLogOnly(error, true));
        Assert.False(VideoTuningNoticePolicy.IsLogOnly(error, false));
    }
    [Theory]
    [InlineData("network-video-rejected", "H264 codec unavailable")]
    [InlineData("network-video-rejected", "Video parameters unavailable")]
    [InlineData("video-preferences-failed", "Codec defaults rejected")]
    [InlineData("camera-capture-failed", "Encoder rejected bitrate change")]
    [InlineData("desktop-capture-failed", "Desktop produced no first frame within 5 seconds")]
    public void Actual_failures_are_never_hidden(string code, string detail) =>
        Assert.False(VideoTuningNoticePolicy.IsLogOnly(new MediaError(code, detail), true));
}
