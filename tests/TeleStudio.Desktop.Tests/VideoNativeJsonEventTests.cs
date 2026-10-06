using System.Text;
using TeleStudio.Contracts;
using TeleStudio.Desktop;

namespace TeleStudio.Desktop.Tests;

public sealed class VideoNativeJsonEventTests
{
    [Fact]
    public void Video_state_and_local_surface_parse_exact_ordered_wire_records()
    {
        var state = "{\"schema\":1,\"type\":\"video-state\",\"sequence\":7,\"protocol\":\"sip\",\"cameraRequested\":true,\"desktopRequested\":false,\"cameraActive\":true,\"desktopActive\":false,\"resolution\":\"hd\",\"bitrateKbps\":null,\"appliedWidth\":1280,\"appliedHeight\":720,\"appliedBitrateKbps\":0,\"overlaySize\":\"medium\",\"overlayPosition\":\"bottom-right\",\"revision\":\"9\",\"cameraError\":\"\",\"desktopError\":\"\"}\n"u8;
        Assert.True(NativeJsonEvent.TryParse(state, CallProtocol.Sip, out var parsed, out var reason), reason);
        Assert.Equal(new VideoStateChanged(true, false, true, false, VideoResolution.Hd, null, 1280, 720, 0, CameraOverlaySize.Medium, CameraOverlayPosition.BottomRight, 9, "", ""), parsed!.Event);
        var local = "{\"schema\":1,\"type\":\"local-surface\",\"sequence\":8,\"protocol\":\"sip\",\"hostHandle\":\"1\",\"width\":1280,\"height\":720,\"format\":\"bgra8-unorm\",\"epoch\":\"2\"}\n"u8;
        Assert.True(NativeJsonEvent.TryParse(local, CallProtocol.Sip, out var surface, out reason), reason);
        Assert.Equal(new LocalSurfaceChanged(1, 1280, 720, 2), surface!.Event);
    }

    [Theory]
    [InlineData("{\"schema\":1,\"type\":\"video-state\",\"sequence\":7,\"protocol\":\"sip\",\"cameraRequested\":true,\"desktopRequested\":false,\"cameraActive\":true,\"desktopActive\":false,\"resolution\":\"hd\",\"bitrateKbps\":127,\"appliedWidth\":1280,\"appliedHeight\":720,\"appliedBitrateKbps\":0,\"overlaySize\":\"medium\",\"overlayPosition\":\"bottom-right\",\"revision\":\"9\",\"cameraError\":\"\",\"desktopError\":\"\"}\n")]
    public void Invalid_video_state_is_rejected(string input) =>
        Assert.False(NativeJsonEvent.TryParse(Encoding.UTF8.GetBytes(input), CallProtocol.Sip, out _, out _));
}
