using MyStudio.WinUI;
using TeleStudio.Contracts;

namespace TeleStudio.Desktop.Tests;

public sealed class OpalSharingPolicyTests
{
    private static VideoStateChanged State(bool camera = false, bool desktop = false, bool active = false) =>
        new(camera, desktop, camera && active, desktop && active, VideoResolution.Hd, null,
            0, 0, 0, CameraOverlaySize.Medium, CameraOverlayPosition.BottomRight, 1, "", "");

    [Fact]
    public void A_new_connected_call_has_no_source_to_stop_and_selection_is_required()
    {
        var controls = OpalSharingPolicy.Controls(true, true, false, null);
        Assert.True(controls.CanChoose);
        Assert.False(controls.CanStop);
        Assert.False(controls.Active);
        Assert.Empty(OpalSharingPolicy.Stop(null));
        Assert.Throws<ArgumentNullException>(() => OpalSharingPolicy.Select(null!, null));
    }

    [Fact]
    public void Explicit_camera_selection_stops_screen_sharing_before_enabling_the_selected_device()
    {
        var selected = new CaptureCatalogItem("camera-id", CaptureItemKind.Camera, "Camera", 0, 0);
        Assert.Equal(new EngineCommand[] { new SetDesktopEnabled(false), new SetCameraEnabled(true, "camera-id") },
            OpalSharingPolicy.Select(selected, State(desktop: true, active: true)));
    }

    [Theory]
    [InlineData(CaptureItemKind.Monitor)]
    [InlineData(CaptureItemKind.Window)]
    public void Explicit_display_selection_stops_camera_before_enabling_the_selected_source(CaptureItemKind kind)
    {
        var selected = new CaptureCatalogItem("source-id", kind, "Source", 1920, 1080);
        Assert.Equal(new EngineCommand[] { new SetCameraEnabled(false), new SetDesktopEnabled(true, "source-id") },
            OpalSharingPolicy.Select(selected, State(camera: true, active: true)));
    }

    [Fact]
    public void Changing_camera_releases_the_previous_device_and_stop_is_reversible()
    {
        var selected = new CaptureCatalogItem("new-camera", CaptureItemKind.Camera, "Camera", 0, 0);
        Assert.Equal(new EngineCommand[] { new SetCameraEnabled(false), new SetCameraEnabled(true, "new-camera") },
            OpalSharingPolicy.Select(selected, State(camera: true, active: true)));
        Assert.Equal(new EngineCommand[] { new SetCameraEnabled(false) }, OpalSharingPolicy.Stop(State(camera: true)));
        Assert.Empty(OpalSharingPolicy.Stop(State()));
    }

    [Theory]
    [InlineData(false, true, false)]
    [InlineData(true, false, false)]
    [InlineData(true, true, true)]
    public void Disconnected_unsupported_or_busy_controls_cannot_share_or_stop(bool connected, bool supported, bool busy)
    {
        var controls = OpalSharingPolicy.Controls(connected, supported, busy, State(camera: true, active: true));
        Assert.False(controls.CanChoose);
        Assert.False(controls.CanStop);
        Assert.Equal(connected && supported, controls.Active);
    }

    [Fact]
    public void Requested_camera_can_be_stopped_without_claiming_confirmed_capture()
    {
        var pending = OpalSharingPolicy.Controls(true, true, false, State(camera: true));
        Assert.True(pending.CanStop);
        Assert.False(pending.Active);
        var confirmed = OpalSharingPolicy.Controls(true, true, false, State(camera: true, active: true));
        Assert.True(confirmed.Active);
    }
}
