using System.ComponentModel;
using TeleStudio.Contracts;
using TeleStudio.Desktop;

namespace MyStudio.WinUI;

public sealed partial class MainWindow
{
    private InCallAudioViewModel? callAudio;
    private DebouncedAudioCommitScheduler? audioCommitScheduler;
    private bool audioRenderQueued;

    private void InitializeAudioControls()
    {
        StopAudioControls();
        if (session.Engine is null) return;
        audioCommitScheduler = new();
        callAudio = new(session.Engine, audioCommitScheduler, new UiAudioTimeoutScheduler(this));
        callAudio.PropertyChanged += AudioControlsChanged;
    }
    private void StopAudioControls()
    {
        if (callAudio is { } audio)
        {
            audio.PropertyChanged -= AudioControlsChanged;
            audio.UpdateAvailability(false, false);
        }
        callAudio = null; audioCommitScheduler?.Dispose(); audioCommitScheduler = null;
    }
    private void AudioControlsChanged(object? sender, PropertyChangedEventArgs args)
    {
        if (audioRenderQueued || closing) return;
        audioRenderQueued = true;
        DispatcherQueue.TryEnqueue(() => { audioRenderQueued = false; if (!closing) Render(); });
    }
    private void ApplyAudioControlEvent(EngineEvent value)
    {
        if (callAudio is not { } audio) return;
        if (value is CallConnected or MediaCapabilitiesChanged or IncomingCall or CallEnded or CallFailed)
            audio.UpdateAvailability(session.Calls?.IsConnected == true, capabilities?.AudioControl == true,
                capabilities?.MuteControl == true, capabilities?.SystemAudioControl == true, capabilities?.RoomMixerControl == true);
        switch (value)
        {
            case MicrophoneMuteChanged state: audio.ApplyMicrophoneMuteConfirmation(state); break;
            case RoomAudioMuteChanged state: audio.ApplyRoomAudioMuteConfirmation(state); break;
            case SystemAudioChanged state: audio.ApplySystemAudioConfirmation(state); break;
            case MediaError error: audio.ApplyMediaError(error); break;
        }
    }
    private Task ToggleMicrophoneAsync() => callAudio?.ToggleMicrophoneMuteAsync(lifetime.Token) ?? Task.CompletedTask;
    private Task ToggleRoomAudioAsync() => callAudio?.ToggleRoomMuteAsync(lifetime.Token) ?? Task.CompletedTask;
    private Task ToggleSystemAudioAsync() => callAudio?.ToggleSystemAudioMuteAsync(lifetime.Token) ?? Task.CompletedTask;

    private sealed class UiAudioTimeoutScheduler(MainWindow owner) : IAudioConfirmationTimeoutScheduler
    {
        public IDisposable Schedule(TimeSpan delay, Action action)
        {
            Timer? timer = null;
            timer = new(_ => { timer?.Dispose(); owner.DispatcherQueue.TryEnqueue(() => { if (!owner.closing) action(); }); }, null, delay, Timeout.InfiniteTimeSpan);
            return timer;
        }
    }
}
