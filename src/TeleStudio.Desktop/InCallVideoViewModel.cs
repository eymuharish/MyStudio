using System.ComponentModel;
using TeleStudio.Contracts;
using TeleStudio.Presentation;
using TeleStudio.Settings;

namespace TeleStudio.Desktop;

/// <summary>Confirmed media state and four independent command lanes; preview state is presentation-only.</summary>
public sealed class InCallVideoViewModel(IEngineCommandSink engine) : INotifyPropertyChanged
{
    public event PropertyChangedEventHandler? PropertyChanged;
    private static VideoStateChanged Empty => new(false, false, false, false, VideoResolution.Auto, null,
        0, 0, 0, CameraOverlaySize.Medium, CameraOverlayPosition.BottomRight, 0, "", "");
    private bool connected;
    private long generation;
    private bool? cameraTarget, desktopTarget;
    private SetVideoPreferences? preferenceTarget;
    private SetCameraOverlay? layoutTarget;
    private string? commandError;
    public VideoStateChanged Confirmed { get; private set; } = Empty;
    public bool Available { get; private set; }
    public bool CameraPending => cameraTarget.HasValue;
    public bool DesktopPending => desktopTarget.HasValue;
    public bool PreferencesPending => preferenceTarget is not null;
    public bool LayoutPending => layoutTarget is not null;
    public bool CanSelectCamera => !CameraPending && !Confirmed.CameraRequested && !Confirmed.CameraActive;
    public bool CanSelectDesktop => !DesktopPending && !Confirmed.DesktopRequested && !Confirmed.DesktopActive;
    public bool CanToggleCamera => Available && !CameraPending;
    public bool CanToggleDesktop => Available && !DesktopPending &&
        (Confirmed.DesktopRequested || !string.IsNullOrEmpty(DesktopId));
    public string? CameraId { get; private set; }
    public string? DesktopId { get; private set; }
    public VideoResolution RequestedResolution { get; private set; }
    public int? RequestedBitrateKbps { get; private set; }
    public CameraOverlaySize PreferredSize { get; private set; } = CameraOverlaySize.Medium;
    public CameraOverlayPosition PreferredPosition { get; private set; } = CameraOverlayPosition.BottomRight;
    public bool ShowOutgoing { get; private set; }
    public bool PictureInPicture { get; private set; }
    public string? Error { get; private set; }

    public void LoadSettings(AppSettings settings)
    {
        CameraId = string.IsNullOrEmpty(settings.CameraId) ? null : settings.CameraId;
        RequestedResolution = settings.VideoResolution; RequestedBitrateKbps = settings.VideoBitrateKbps;
        PreferredSize = settings.CameraOverlaySize; PreferredPosition = settings.CameraOverlayPosition;
        Notify();
    }
    public void SelectCamera(string? id)
    {
        if (!CanSelectCamera) throw new InvalidOperationException("Stop the camera before selecting a device.");
        CameraId = string.IsNullOrEmpty(id) ? null : id; Notify();
    }
    public void SelectDesktop(string? id)
    {
        if (!CanSelectDesktop) throw new InvalidOperationException("Stop sharing before selecting a source.");
        DesktopId = id; Notify();
    }
    public void UpdateAvailability(bool isConnected, bool supported)
    {
        if (connected != isConnected)
        {
            generation++; Confirmed = Empty; cameraTarget = desktopTarget = null;
            preferenceTarget = null; layoutTarget = null; Error = null; commandError = null;
        }
        connected = isConnected; Available = isConnected && supported; Notify();
    }
    public async Task ToggleCameraAsync(CancellationToken token)
    {
        if (!CanToggleCamera) throw new InvalidOperationException("Camera control is not ready.");
        cameraTarget = !Confirmed.CameraRequested; Error = null; Notify();
        var operation = generation;
        try { await engine.SendAsync(new SetCameraEnabled(cameraTarget.Value, cameraTarget.Value ? CameraId : null), token); }
        catch { if (operation == generation) { cameraTarget = null; Notify(); } throw; }
    }
    public async Task ToggleDesktopAsync(CancellationToken token)
    {
        if (!CanToggleDesktop) throw new InvalidOperationException("Desktop control is not ready.");
        desktopTarget = !Confirmed.DesktopRequested; Error = null; Notify();
        var operation = generation;
        try { await engine.SendAsync(new SetDesktopEnabled(desktopTarget.Value, desktopTarget.Value ? DesktopId : null), token); }
        catch { if (operation == generation) { desktopTarget = null; Notify(); } throw; }
    }
    public async Task SetPreferencesAsync(VideoResolution resolution, int? bitrate, CancellationToken token)
    {
        if (PreferencesPending || connected && !Available) throw new InvalidOperationException("Video preferences are not ready.");
        var command = new SetVideoPreferences(resolution, bitrate);
        commandError = null;
        RequestedResolution = resolution; RequestedBitrateKbps = bitrate;
        preferenceTarget = connected ? command : null; Error = null; Notify();
        var operation = generation;
        try { await engine.SendAsync(command, token); }
        catch { if (operation == generation) { preferenceTarget = null; Notify(); } throw; }
    }
    public async Task SetOverlayAsync(CameraOverlaySize size, CameraOverlayPosition position, CancellationToken token)
    {
        if (!Available || LayoutPending) throw new InvalidOperationException("Camera layout is not ready.");
        var command = new SetCameraOverlay(size, position);
        commandError = null;
        _ = VideoCommandCodec.Format(command);
        layoutTarget = command; Error = null; Notify(); var operation = generation;
        try { await engine.SendAsync(command, token); }
        catch { if (operation == generation) { layoutTarget = null; Notify(); } throw; }
    }
    public void ApplyState(VideoStateChanged state)
    {
        if (!Available || state.Revision <= Confirmed.Revision) return;
        Confirmed = state;
        if (cameraTarget is bool camera && (camera && state.CameraRequested && state.CameraError.Length != 0 ||
            state.CameraRequested == camera && state.CameraActive == camera)) cameraTarget = null;
        if (desktopTarget is bool desktop && (desktop && state.DesktopRequested && state.DesktopError.Length != 0 ||
            state.DesktopRequested == desktop && state.DesktopActive == desktop)) desktopTarget = null;
        if (preferenceTarget is { } preferences && preferences.Resolution == state.Resolution && preferences.BitrateKbps == state.BitrateKbps)
            preferenceTarget = null;
        if (layoutTarget is { } layout && layout.Size == state.OverlaySize && layout.Position == state.OverlayPosition)
        {
            layoutTarget = null; PreferredSize = state.OverlaySize; PreferredPosition = state.OverlayPosition;
        }
        Error = state.CameraError.Length != 0 ? state.CameraError : state.DesktopError.Length != 0 ? state.DesktopError : commandError;
        Notify();
    }
    public void ApplyError(MediaError error)
    {
        if (!connected) return;
        if (VideoTuningNoticePolicy.IsLogOnly(error, connected))
        {
            if (error.Code == "video-preferences-failed") preferenceTarget = null;
            // Rejected changes retain the last confirmed stream. Do not clear a separate capture failure.
            Notify();
            return;
        }
        switch (error.Code)
        {
            case "video-preferences-failed": preferenceTarget = null; break;
            case "camera-overlay-failed": layoutTarget = null; break;
            case "camera-capture-failed": if (cameraTarget == true) cameraTarget = null; break;
            case "desktop-capture-failed": if (desktopTarget == true) desktopTarget = null; break;
            case "video-output-not-ready": cameraTarget = desktopTarget = null; preferenceTarget = null; layoutTarget = null; break;
            default: return;
        }
        commandError = error.Code is "video-preferences-failed" or "camera-overlay-failed" or "video-output-not-ready" ? error.Detail : null;
        Error = error.Detail; Notify();
    }
    public void ToggleView() { ShowOutgoing = !ShowOutgoing; Notify(); }
    public void TogglePictureInPicture() { PictureInPicture = !PictureInPicture; Notify(); }
    private void Notify() => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(null));
}
