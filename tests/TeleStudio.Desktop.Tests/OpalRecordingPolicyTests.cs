using MyStudio.WinUI;
using TeleStudio.Contracts;

namespace TeleStudio.Desktop.Tests;

public sealed class OpalRecordingPolicyTests
{
    [Theory]
    [InlineData(CaptureItemKind.Monitor)]
    [InlineData(CaptureItemKind.Window)]
    public void Paused_recording_resumes_only_after_explicit_sharing_is_confirmed(CaptureItemKind kind)
    {
        var paused = new RecordingStateChanged(RecordingState.Recording, true, true, 1300, "", true);
        Assert.False(OpalRecordingPolicy.CanPauseOrResume(paused, State(false, false), Source(kind)));
        Assert.False(OpalRecordingPolicy.CanPauseOrResume(paused, State(true, false), Source(kind)));
        Assert.Throws<InvalidOperationException>(() => OpalRecordingPolicy.SetPaused(paused, false, State(true, false), Source(kind)));
        Assert.True(OpalRecordingPolicy.CanPauseOrResume(paused, State(true, true), Source(kind)));
        Assert.Equal(new SetLectureRecordingPaused(false), OpalRecordingPolicy.SetPaused(paused, false, State(true, true), Source(kind)));
    }

    [Fact]
    public void Camera_or_missing_selection_cannot_resume_but_manual_pause_stays_available()
    {
        var paused = new RecordingStateChanged(RecordingState.Recording, true, true, 1300, "", true);
        Assert.False(OpalRecordingPolicy.CanPauseOrResume(paused, State(true, true), Source(CaptureItemKind.Camera)));
        Assert.False(OpalRecordingPolicy.CanPauseOrResume(paused, State(true, true), null));
        Assert.Throws<InvalidOperationException>(() => OpalRecordingPolicy.SetPaused(paused, false, State(true, true), Source(CaptureItemKind.Camera)));
        Assert.Equal(new SetLectureRecordingPaused(true), OpalRecordingPolicy.SetPaused(paused with { Paused = false }, true, null, null));
        Assert.False(OpalRecordingPolicy.CanPauseOrResume(paused with { State = RecordingState.Completed }, State(true, true), Source()));
    }
    private static VideoStateChanged State(bool requested, bool active) => new(false, requested, false, active,
        VideoResolution.Fhd, null, 1920, 1080, 4000, CameraOverlaySize.Medium, CameraOverlayPosition.BottomRight, 1, "", "");
    private static CaptureCatalogItem Source(CaptureItemKind kind = CaptureItemKind.Window) => new("exact-shared-id", kind, "Shared source", 1920, 1080);

    [Theory]
    [InlineData(CaptureItemKind.Monitor)]
    [InlineData(CaptureItemKind.Window)]
    public void Recording_uses_the_exact_confirmed_shared_source(CaptureItemKind kind)
    {
        var source = Source(kind);
        Assert.True(OpalRecordingPolicy.IsSharedDesktopActive(State(true, true), source));
        Assert.Equal(new StartLectureRecording(source.Id, false, true), OpalRecordingPolicy.Start(true, State(true, true), source, false, true));
        var capabilities = new MediaCapabilitiesChanged(true, true, true, true, true, true);
        Assert.True(StudioControlPolicy.From(true, capabilities, RecordingState.Idle, true).Record);
        Assert.False(StudioControlPolicy.From(true, capabilities, RecordingState.Recording, true).Record);
        Assert.False(StudioControlPolicy.From(false, capabilities, RecordingState.Idle, true).Record);
    }

    [Theory]
    [InlineData(false, false)]
    [InlineData(true, false)]
    [InlineData(false, true)]
    public void Requested_or_stopped_sharing_is_not_recordable(bool requested, bool active)
    {
        Assert.False(OpalRecordingPolicy.IsSharedDesktopActive(State(requested, active), Source()));
        Assert.Equal(OpalRecordingPolicy.SharingRequired,
            Assert.Throws<InvalidOperationException>(() => OpalRecordingPolicy.Start(true, State(requested, active), Source())).Message);
    }

    [Fact]
    public void Camera_or_no_explicit_selection_never_falls_back_to_a_display()
    {
        Assert.False(OpalRecordingPolicy.IsSharedDesktopActive(State(true, true), Source(CaptureItemKind.Camera)));
        Assert.Throws<InvalidOperationException>(() => OpalRecordingPolicy.Start(true, State(true, true), Source(CaptureItemKind.Camera)));
        Assert.Throws<InvalidOperationException>(() => OpalRecordingPolicy.Start(true, State(true, true), null));
        Assert.Throws<InvalidOperationException>(() => OpalRecordingPolicy.Start(false, State(true, true), Source()));
    }
}
