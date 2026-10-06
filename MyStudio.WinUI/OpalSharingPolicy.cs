using TeleStudio.Contracts;
using TeleStudio.Desktop;

namespace MyStudio.WinUI;

public sealed record SharingControlState(bool CanChoose, bool CanStop, bool Active);

public static class OpalSharingPolicy
{
    public static SharingControlState Controls(bool connected, bool supported, bool busy, VideoStateChanged? state)
    {
        var available = connected && supported;
        var requested = state is { CameraRequested: true } or { DesktopRequested: true };
        var active = available && (state is { CameraActive: true } or { DesktopActive: true });
        return new(available && !busy, available && !busy && (requested || active), active);
    }

    public static IReadOnlyList<EngineCommand> Select(CaptureCatalogItem selected, VideoStateChanged? state, SharingTransmissionMode mode = SharingTransmissionMode.Auto)
    {
        ArgumentNullException.ThrowIfNull(selected);
        if (!Enum.IsDefined(mode)) throw new ArgumentOutOfRangeException(nameof(mode));
        if (string.IsNullOrWhiteSpace(selected.Id)) throw new ArgumentException("Choose a sharing source.", nameof(selected));
        if (selected.Kind is not (CaptureItemKind.Camera or CaptureItemKind.Monitor or CaptureItemKind.Window))
            throw new ArgumentOutOfRangeException(nameof(selected));
        var commands = Stop(state).ToList();
        commands.Add(selected.Kind == CaptureItemKind.Camera
            ? new SetCameraEnabled(true, selected.Id) : new SetDesktopEnabled(true, selected.Id, mode));
        return commands;
    }

    public static IReadOnlyList<EngineCommand> Stop(VideoStateChanged? state)
    {
        var commands = new List<EngineCommand>();
        if (state is { CameraRequested: true } or { CameraActive: true }) commands.Add(new SetCameraEnabled(false));
        if (state is { DesktopRequested: true } or { DesktopActive: true }) commands.Add(new SetDesktopEnabled(false));
        return commands;
    }
}
