using MyStudio.WinUI;
using TeleStudio.Contracts;

namespace TeleStudio.Desktop.Tests;

public sealed class OpalScreenSharingPauseTests
{
    private static VideoStateChanged State(bool requested = false, bool active = false, bool camera = false) =>
        new(camera, requested, camera, active, VideoResolution.Fhd, null, 1920, 1080, 4000,
            CameraOverlaySize.Medium, CameraOverlayPosition.BottomRight, 1, "", "");

    [Theory]
    [InlineData(CaptureItemKind.Monitor)]
    [InlineData(CaptureItemKind.Window)]
    public void Pause_remembers_only_the_selected_shared_source_and_resume_needs_a_fresh_ack(CaptureItemKind kind)
    {
        var model = new OpalScreenSharingPause();
        var selected = new CaptureCatalogItem("chosen-exact-source", kind, "Chosen", 1920, 1080);
        Assert.Equal(new SetDesktopEnabled(false), model.Pause(selected, State(true, true)));
        Assert.Equal(selected, model.RememberedSource);
        Assert.False(model.Controls(true, true, false, false, State(true, true), null).CanToggle);
        Assert.True(model.Controls(true, true, false, false, State(), null).CanToggle);
        Assert.Equal(selected, model.RequestResume(State()));
        Assert.True(model.ResumeRequested);
        Assert.False(model.Controls(true, true, false, false, State(true), selected).CanToggle);
        model.ConfirmResume(State(true), selected);
        Assert.True(model.ResumeRequested);
        model.ConfirmResume(State(true, true), selected with { Id = "different" });
        Assert.True(model.ResumeRequested);
        model.ConfirmResume(State(true, true), selected);
        Assert.Null(model.RememberedSource);
        Assert.False(model.ResumeRequested);
        Assert.True(model.Controls(true, true, false, false, State(true, true), selected).CanToggle);
    }

    [Fact]
    public void Camera_default_sources_and_unconfirmed_sharing_cannot_be_remembered()
    {
        var model = new OpalScreenSharingPause();
        var camera = new CaptureCatalogItem("camera", CaptureItemKind.Camera, "Camera", 0, 0);
        var display = camera with { Kind = CaptureItemKind.Monitor };
        Assert.Throws<InvalidOperationException>(() => model.Pause(null, State(true, true)));
        Assert.Throws<InvalidOperationException>(() => model.Pause(camera, State(true, true)));
        Assert.Throws<InvalidOperationException>(() => model.Pause(display, State(true)));
        Assert.Throws<InvalidOperationException>(() => model.Pause(display, State(true, true, camera: true)));
        Assert.Throws<InvalidOperationException>(() => model.RequestResume(State()));
        Assert.Null(model.RememberedSource);
        Assert.False(model.Controls(true, true, false, false, State(camera: true), camera).CanToggle);
    }

    [Theory]
    [InlineData(false, true, false, false)]
    [InlineData(true, false, false, false)]
    [InlineData(true, true, true, false)]
    [InlineData(true, true, false, true)]
    public void Disconnected_unsupported_busy_or_held_call_cannot_toggle(bool connected, bool supported, bool busy, bool held)
    {
        var model = new OpalScreenSharingPause();
        var display = new CaptureCatalogItem("chosen", CaptureItemKind.Monitor, "Chosen", 1920, 1080);
        Assert.False(model.Controls(connected, supported, busy, held, State(true, true), display).CanToggle);
        model.Pause(display, State(true, true));
        Assert.False(model.Controls(connected, supported, busy, held, State(), null).CanToggle);
    }

    [Fact]
    public void Selecting_another_source_or_ending_the_call_clears_resume_memory_and_recording_stays_paused()
    {
        var model = new OpalScreenSharingPause();
        var display = new CaptureCatalogItem("chosen", CaptureItemKind.Window, "Chosen", 1920, 1080);
        model.Pause(display, State(true, true));
        var recording = new RecordingStateChanged(RecordingState.Recording, true, true, 700, "", true);
        var resumed = model.RequestResume(State());
        model.ConfirmResume(State(true, true), resumed);
        Assert.True(recording.Paused);
        Assert.True(OpalRecordingPolicy.CanPauseOrResume(recording, State(true, true), resumed));
        Assert.Equal(new SetLectureRecordingPaused(false), OpalRecordingPolicy.SetPaused(recording, false, State(true, true), resumed));
        model.Pause(display, State(true, true)); model.Clear();
        Assert.Throws<InvalidOperationException>(() => model.RequestResume(State()));
        Assert.False(model.Controls(true, true, false, false, State(), null).CanToggle);
    }
}
