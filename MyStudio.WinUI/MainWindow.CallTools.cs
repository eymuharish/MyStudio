using Microsoft.UI.Xaml.Controls;

namespace MyStudio.WinUI;

public sealed partial class MainWindow
{
    private readonly Dictionary<AppBarButton, (Symbol Symbol, CallToolState State)> renderedCallTools = [];
    private void RenderCallToolStates()
    {
        void Apply(AppBarButton? button, Symbol symbol, string name, string offAction, string onAction, bool off, bool? requested = null, string? error = null)
        {
            if (button is null) return;
            var state = CallToolState.Create(name, offAction, onAction, off, button.IsEnabled, arabic, requested, error);
            if (renderedCallTools.TryGetValue(button, out var previous) && previous == (symbol, state) && button.Label == state.Label) return;
            CallControlVisuals.Apply(button, symbol, state); renderedCallTools[button] = (symbol, state);
        }
        Apply(microphone, Symbol.Microphone, T("ميكروفون", "Mic"), T("كتم الميكروفون", "Mute microphone"), T("تشغيل الميكروفون", "Unmute microphone"), micMuted,
            callAudio?.IsMicrophoneMutePending == true ? !callAudio.PendingMicrophoneMuted : null);
        Apply(room, Symbol.Volume, T("صوت الغرفة", "Room"), T("كتم صوت الغرفة", "Mute room audio"), T("تشغيل صوت الغرفة", "Unmute room audio"), roomMuted,
            callAudio?.IsRoomMutePending == true ? !callAudio.PendingRoomMuted : null);
        if (shareFrom is not null) CallControlVisuals.Apply(shareFrom, Symbol.Share,
            new(T("مشاركة من", "Share from"), T("اختر كاميرا أو شاشة أو نافذة لمشاركتها", "Choose a camera, display or window to share"), false, false));
        if (stopSharing is not null)
        {
            var active = videoState is { CameraActive: true } or { DesktopActive: true };
            var detail = active ? T("المشاركة تعمل. إيقاف المشاركة", "Sharing is active. Stop sharing")
                : T("المشاركة لم تُؤكد بعد. إيقاف المشاركة", "Sharing is not yet confirmed. Stop sharing");
            if (!string.IsNullOrEmpty(videoState?.CameraError)) detail += ". " + videoState.CameraError;
            if (!string.IsNullOrEmpty(videoState?.DesktopError)) detail += ". " + videoState.DesktopError;
            CallControlVisuals.Apply(stopSharing, Symbol.Stop, new(T("إيقاف المشاركة", "Stop sharing"), detail, false, active));
        }
        Apply(systemAudio, Symbol.Audio, T("صوت النظام", "System"), T("إيقاف صوت النظام", "Stop system audio"), T("مشاركة صوت النظام", "Share system audio"), !systemEnabled,
            callAudio?.IsSystemAudioPending == true ? !callAudio.PendingSystemAudioMuted : null, callAudio?.LastErrorMessage);
    }
}
