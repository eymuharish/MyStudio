using TeleStudio.Contracts;
using TeleStudio.Desktop;

namespace MyStudio.WinUI;

public static class OpalRecordingPolicy
{
    public const string SharingRequired = "Share a display or window before recording.";

    public static bool IsSharedDesktopActive(VideoStateChanged? state, CaptureCatalogItem? selected) =>
        state is { DesktopRequested: true, DesktopActive: true } &&
        selected is { Kind: CaptureItemKind.Monitor or CaptureItemKind.Window } &&
        !string.IsNullOrWhiteSpace(selected.Id);

    public static StartLectureRecording Start(bool canRecord, VideoStateChanged? state, CaptureCatalogItem? selected,
        bool microphone = true, bool roomAudio = true)
    {
        if (!IsSharedDesktopActive(state, selected)) throw new InvalidOperationException(SharingRequired);
        if (!canRecord) throw new InvalidOperationException("Recording is unavailable or already running.");
        return new(selected!.Id, microphone, roomAudio);
    }

    public static bool CanPauseOrResume(RecordingStateChanged recording, VideoStateChanged? state, CaptureCatalogItem? selected) =>
        recording.State == RecordingState.Recording && (!recording.Paused || IsSharedDesktopActive(state, selected));

    public static SetLectureRecordingPaused SetPaused(RecordingStateChanged recording, bool paused,
        VideoStateChanged? state, CaptureCatalogItem? selected)
    {
        if (recording.State != RecordingState.Recording) throw new InvalidOperationException("An active recording is required.");
        if (!paused && !IsSharedDesktopActive(state, selected)) throw new InvalidOperationException(SharingRequired);
        return new(paused);
    }
}
