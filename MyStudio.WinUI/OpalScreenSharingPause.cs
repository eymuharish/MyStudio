using TeleStudio.Contracts;
using TeleStudio.Desktop;

namespace MyStudio.WinUI;

public sealed record ScreenSharingPauseView(bool CanToggle, bool Paused, bool Resuming);

/// <summary>Remembers an explicit screen/window choice only until resume is acknowledged or the choice is replaced.</summary>
public sealed class OpalScreenSharingPause
{
    public CaptureCatalogItem? RememberedSource { get; private set; }
    public bool ResumeRequested { get; private set; }

    private static bool CameraOff(VideoStateChanged? state) => state is { CameraRequested: false, CameraActive: false };
    private bool CanResume(VideoStateChanged? state) => RememberedSource is not null && !ResumeRequested &&
        CameraOff(state) && state is { DesktopRequested: false, DesktopActive: false };

    public ScreenSharingPauseView Controls(bool connected, bool supported, bool busy, bool held,
        VideoStateChanged? state, CaptureCatalogItem? selected)
    {
        var enabled = connected && supported && !busy && !held &&
            (RememberedSource is not null ? CanResume(state) : CameraOff(state) && OpalRecordingPolicy.IsSharedDesktopActive(state, selected));
        return new(enabled, RememberedSource is not null, ResumeRequested);
    }

    public SetDesktopEnabled Pause(CaptureCatalogItem? selected, VideoStateChanged? state)
    {
        if (!CameraOff(state) || !OpalRecordingPolicy.IsSharedDesktopActive(state, selected))
            throw new InvalidOperationException("Choose and share a display or window before pausing sharing.");
        RememberedSource = selected; ResumeRequested = false;
        return new(false);
    }

    public CaptureCatalogItem RequestResume(VideoStateChanged? state)
    {
        if (!CanResume(state)) throw new InvalidOperationException("Wait for screen sharing to stop before resuming the selected source.");
        ResumeRequested = true;
        return RememberedSource!;
    }

    public void ConfirmResume(VideoStateChanged? state, CaptureCatalogItem? selected)
    {
        if (ResumeRequested && selected?.Id == RememberedSource?.Id && CameraOff(state) &&
            OpalRecordingPolicy.IsSharedDesktopActive(state, selected)) Clear();
    }

    public void CancelResumeRequest() => ResumeRequested = false;
    public void Clear() { RememberedSource = null; ResumeRequested = false; }
}
