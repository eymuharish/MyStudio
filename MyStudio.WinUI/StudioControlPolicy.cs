using TeleStudio.Contracts;

namespace MyStudio.WinUI;

public sealed record StudioControlPolicy(bool Camera, bool Audio, bool Record, bool StopRecording)
{
    public static StudioControlPolicy From(bool connected, MediaCapabilitiesChanged? capabilities, RecordingState recording, bool sharedDesktopActive = false) =>
        new(connected && capabilities is { SourceControl: true, VideoControl: true },
            connected && capabilities?.AudioControl == true,
            connected && capabilities?.Recording == true && sharedDesktopActive && recording is RecordingState.Idle or RecordingState.Completed or RecordingState.Failed,
            recording is RecordingState.Starting or RecordingState.Recording);
}
