using System.ComponentModel;
using System.Runtime.CompilerServices;
using TeleStudio.Contracts;
using TeleStudio.Presentation;

namespace TeleStudio.Desktop;

public interface IAudioCommitScheduler
{
    void Schedule(Func<Task> action);
    void Cancel();
}

public interface IAudioConfirmationTimeoutScheduler
{
    IDisposable Schedule(TimeSpan delay, Action action);
}

public sealed class AudioConfirmationTimeoutScheduler : IAudioConfirmationTimeoutScheduler
{
    private readonly SynchronizationContext? _context = SynchronizationContext.Current;

    public IDisposable Schedule(TimeSpan delay, Action action)
    {
        ArgumentNullException.ThrowIfNull(action);
        Timer? timer = null;
        timer = new Timer(_ =>
        {
            timer?.Dispose();
            if (_context is null) action();
            else _context.Post(_ => action(), null);
        }, null, delay, Timeout.InfiniteTimeSpan);
        return timer;
    }
}

public sealed class DebouncedAudioCommitScheduler : IAudioCommitScheduler, IDisposable
{
    private readonly TimeSpan _delay;
    private CancellationTokenSource? _pending;

    public DebouncedAudioCommitScheduler(TimeSpan? delay = null) => _delay = delay ?? TimeSpan.FromMilliseconds(180);
    public void Schedule(Func<Task> action)
    {
        ArgumentNullException.ThrowIfNull(action);
        Cancel();
        var source = new CancellationTokenSource();
        _pending = source;
        _ = RunAsync(action, source);
    }
    public void Cancel()
    {
        var source = Interlocked.Exchange(ref _pending, null);
        if (source is null) return;
        source.Cancel();
        source.Dispose();
    }
    public void Dispose() => Cancel();
    private async Task RunAsync(Func<Task> action, CancellationTokenSource source)
    {
        try { await Task.Delay(_delay, source.Token); await action(); }
        catch (OperationCanceledException) when (source.IsCancellationRequested) { }
        catch { }
        finally { if (ReferenceEquals(Interlocked.CompareExchange(ref _pending, null, source), source)) source.Dispose(); }
    }
}

public sealed class InCallAudioViewModel : INotifyPropertyChanged
{
    private static readonly TimeSpan ConfirmationTimeout = TimeSpan.FromSeconds(2);
    private enum RoomCommandLane { Mute, Mixer }

    private readonly IEngineCommandSink _engine;
    private readonly IAudioCommitScheduler _scheduler;
    private readonly IAudioConfirmationTimeoutScheduler _confirmationTimeouts;
    private bool _isConnected;
    private bool _isReady;
    private bool _isPending;
    private bool _microphoneReady;
    private bool _systemAudioReady;
    private bool _roomMixerReady;
    private bool _confirmedMicrophoneMuted;
    private bool _pendingMicrophoneMuted;
    private bool _microphoneMutePending;
    private bool _confirmedSystemAudioEnabled;
    private bool _pendingSystemAudioMuted;
    private bool _systemAudioPending;
    private bool _confirmedRoomMuted;
    private bool _pendingRoomMuted;
    private bool _roomMutePending;
    private bool _roomMixerPending;
    private readonly List<RoomCommandLane> _pendingRoomCommands = [];
    private IDisposable? _microphoneMuteTimeout;
    private IDisposable? _systemAudioTimeout;
    private IDisposable? _roomMuteTimeout;
    private IDisposable? _roomMixerTimeout;
    private long _microphoneMuteGeneration;
    private long _systemAudioGeneration;
    private long _roomMuteGeneration;
    private long _roomMixerGeneration;
    private int _confirmedRoomMasterPercent = 100;
    private int _confirmedRoomBassDb;
    private int _confirmedRoomVoiceDb;
    private int _confirmedRoomTrebleDb;
    private int _pendingRoomMasterPercent = 100;
    private int _pendingRoomBassDb;
    private int _pendingRoomVoiceDb;
    private int _pendingRoomTrebleDb;
    private int _roomPreLimiterPeakQ15;
    private int _roomPostLimiterPeakQ15;
    private int _roomLimiterGainReductionQ15;
    private bool _roomLimiterActive;
    private bool _confirmedDesktopSharing;
    private bool _manualSystemMuteForCurrentShare;
    private bool _autoEnableSentForCurrentShare;
    private string? _lastErrorMessage;
    private MediaError? _lastError;
    private int _confirmedRoomVolume = 100;
    private int _confirmedMicrophoneGain = 100;
    private bool _confirmedNoiseIsolation = true;
    private int _pendingRoomVolume = 100;
    private int _pendingMicrophoneGain = 100;
    private bool _pendingNoiseIsolation = true;
    private int _confirmedRoomNoiseReductionDb;
    private int _pendingRoomNoiseReductionDb;

    public InCallAudioViewModel(
        IEngineCommandSink engine,
        IAudioCommitScheduler scheduler,
        IAudioConfirmationTimeoutScheduler? confirmationTimeouts = null)
    {
        _engine = engine ?? throw new ArgumentNullException(nameof(engine));
        _scheduler = scheduler ?? throw new ArgumentNullException(nameof(scheduler));
        _confirmationTimeouts = confirmationTimeouts ?? new AudioConfirmationTimeoutScheduler();
    }

    public event PropertyChangedEventHandler? PropertyChanged;
    public event EventHandler? RoomMeterChanged;
    public bool IsEnabled => _isConnected && _isReady && !_isPending;
    public bool IsPending => _isPending;
    public int ConfirmedRoomVolume => _confirmedRoomVolume;
    public int ConfirmedMicrophoneGain => _confirmedMicrophoneGain;
    public bool ConfirmedNoiseIsolation => _confirmedNoiseIsolation;
    public int PendingRoomVolume => _pendingRoomVolume;
    public int PendingMicrophoneGain => _pendingMicrophoneGain;
    public bool PendingNoiseIsolation => _pendingNoiseIsolation;
    public string NumericFlowDirection => "LeftToRight";
    public string NoiseIsolationDescription => "عزل ضوضاء SpeexDSP / SpeexDSP noise isolation (not AGC)";
    public bool CanToggleMicrophoneMute => _isConnected && _microphoneReady && !_microphoneMutePending;
    public bool CanToggleSystemAudioMute => _isConnected && _systemAudioReady && !_systemAudioPending;
    public bool CanToggleRoomMute => _isConnected && _roomMixerReady && !_roomMutePending;
    public bool CanOpenRoomMixer => _isConnected && _roomMixerReady;
    public bool CanOpenAudioMixer => _isConnected && (_roomMixerReady || _isReady || _microphoneReady);
    public bool ConfirmedMicrophoneMuted => _confirmedMicrophoneMuted;
    public bool PendingMicrophoneMuted => _pendingMicrophoneMuted;
    public bool IsMicrophoneMutePending => _microphoneMutePending;
    public bool ConfirmedSystemAudioEnabled => _confirmedSystemAudioEnabled;
    public bool ConfirmedSystemAudioMuted => !_confirmedSystemAudioEnabled;
    public bool PendingSystemAudioMuted => _pendingSystemAudioMuted;
    public bool IsSystemAudioPending => _systemAudioPending;
    public bool ConfirmedRoomMuted => _confirmedRoomMuted;
    public bool PendingRoomMuted => _pendingRoomMuted;
    public bool IsRoomMutePending => _roomMutePending;
    public bool IsRoomMixerPending => _roomMixerPending;
    public int ConfirmedRoomMasterPercent => _confirmedRoomMasterPercent;
    public int PendingRoomMasterPercent => _pendingRoomMasterPercent;
    public int ConfirmedRoomBassDb => _confirmedRoomBassDb;
    public int PendingRoomBassDb => _pendingRoomBassDb;
    public int ConfirmedRoomVoiceDb => _confirmedRoomVoiceDb;
    public int PendingRoomVoiceDb => _pendingRoomVoiceDb;
    public int ConfirmedRoomTrebleDb => _confirmedRoomTrebleDb;
    public int PendingRoomTrebleDb => _pendingRoomTrebleDb;
    public int ConfirmedRoomNoiseReductionDb => _confirmedRoomNoiseReductionDb;
    public int PendingRoomNoiseReductionDb => _pendingRoomNoiseReductionDb;
    public int RoomPreLimiterPeakQ15 => _roomPreLimiterPeakQ15;
    public int RoomPostLimiterPeakQ15 => _roomPostLimiterPeakQ15;
    public int RoomLimiterGainReductionQ15 => _roomLimiterGainReductionQ15;
    public bool IsLimiterWarningVisible => _roomLimiterActive && _roomLimiterGainReductionQ15 > 0;
    public bool IsHighGainWarningVisible => _pendingRoomMasterPercent > 200;
    public string? LastErrorMessage => _lastErrorMessage;
    public MediaError? LastError => _lastError;

    public void UpdateAvailability(bool connected, bool ready)
        => UpdateAvailability(connected, ready, microphoneReady: false, systemAudioReady: false, roomMixerReady: false);

    public void UpdateAvailability(
        bool connected,
        bool ready,
        bool microphoneReady,
        bool systemAudioReady,
        bool roomMixerReady)
    {
        _isConnected = connected;
        _isReady = ready;
        _microphoneReady = microphoneReady;
        _systemAudioReady = systemAudioReady;
        _roomMixerReady = roomMixerReady;
        if (!connected || !ready)
        {
            _scheduler.Cancel();
            _isPending = false;
            SnapToConfirmed();
        }
        if (!connected)
        {
            ResetIndependentAudio();
        }
        else
        {
            if (!microphoneReady) { _microphoneMutePending = false; _pendingMicrophoneMuted = _confirmedMicrophoneMuted; }
            if (!systemAudioReady) { _systemAudioPending = false; _pendingSystemAudioMuted = !_confirmedSystemAudioEnabled; }
            if (!roomMixerReady)
            {
                _roomMutePending = false;
                _roomMixerPending = false;
                _pendingRoomMuted = _confirmedRoomMuted;
                SnapRoomMixerToConfirmed();
            }
        }
        NotifyAll();
    }

    public async Task ToggleMicrophoneMuteAsync(CancellationToken cancellationToken)
    {
        if (!CanToggleMicrophoneMute) throw new InvalidOperationException("Microphone mute is not ready for this call.");
        _pendingMicrophoneMuted = !_confirmedMicrophoneMuted;
        _microphoneMutePending = true;
        StartMicrophoneMuteTimeout();
        NotifyAll();
        try { await _engine.SendAsync(new SetMicrophoneMuted(_pendingMicrophoneMuted), cancellationToken); }
        catch { CancelMicrophoneMuteTimeout(); _microphoneMutePending = false; _pendingMicrophoneMuted = _confirmedMicrophoneMuted; NotifyAll(); throw; }
    }

    public async Task ToggleSystemAudioMuteAsync(CancellationToken cancellationToken)
    {
        if (!CanToggleSystemAudioMute) throw new InvalidOperationException("System audio is not ready for this call.");
        _pendingSystemAudioMuted = _confirmedSystemAudioEnabled;
        _systemAudioPending = true;
        StartSystemAudioTimeout();
        if (_confirmedDesktopSharing) _manualSystemMuteForCurrentShare = _pendingSystemAudioMuted;
        NotifyAll();
        try { await _engine.SendAsync(new SetSystemAudioEnabled(!_pendingSystemAudioMuted), cancellationToken); }
        catch { CancelSystemAudioTimeout(); _systemAudioPending = false; _pendingSystemAudioMuted = !_confirmedSystemAudioEnabled; NotifyAll(); throw; }
    }

    public async Task ToggleRoomMuteAsync(CancellationToken cancellationToken)
    {
        if (!CanToggleRoomMute) throw new InvalidOperationException("Room mute is not ready for this call.");
        _pendingRoomMuted = !_confirmedRoomMuted;
        _roomMutePending = true;
        TrackRoomCommand(RoomCommandLane.Mute);
        StartRoomMuteTimeout();
        NotifyAll();
        try { await _engine.SendAsync(new SetRoomAudioMuted(_pendingRoomMuted), cancellationToken); }
        catch { CancelRoomMuteTimeout(); UntrackRoomCommand(RoomCommandLane.Mute); _roomMutePending = false; _pendingRoomMuted = _confirmedRoomMuted; NotifyAll(); throw; }
    }

    public Task SetRoomMixerAsync(int masterPercent, int bassDb, int voiceDb, int trebleDb, CancellationToken cancellationToken, int noiseReductionDb = 0)
    {
        ValidateRoomMixer(masterPercent, bassDb, voiceDb, trebleDb);
        ValidateRoomNoiseReduction(noiseReductionDb);
        if (!CanOpenRoomMixer || _roomMixerPending) throw new InvalidOperationException("Room mixer is not ready for this call.");
        _pendingRoomMasterPercent = masterPercent;
        _pendingRoomBassDb = bassDb;
        _pendingRoomVoiceDb = voiceDb;
        _pendingRoomTrebleDb = trebleDb;
        _pendingRoomNoiseReductionDb = noiseReductionDb;
        _roomMixerPending = true;
        TrackRoomCommand(RoomCommandLane.Mixer);
        StartRoomMixerTimeout();
        NotifyAll();
        return SendRoomMixerAsync(cancellationToken);
    }

    public async Task HandleConfirmedSourceChangeAsync(bool isDesktopSharing, CancellationToken cancellationToken)
    {
        if (!isDesktopSharing)
        {
            _confirmedDesktopSharing = false;
            _manualSystemMuteForCurrentShare = false;
            _autoEnableSentForCurrentShare = false;
            return;
        }
        _confirmedDesktopSharing = true;
        if (!CanToggleSystemAudioMute || _confirmedSystemAudioEnabled || _manualSystemMuteForCurrentShare || _autoEnableSentForCurrentShare)
        {
            return;
        }
        _autoEnableSentForCurrentShare = true;
        _pendingSystemAudioMuted = false;
        _systemAudioPending = true;
        StartSystemAudioTimeout();
        NotifyAll();
        try { await _engine.SendAsync(new SetSystemAudioEnabled(true), cancellationToken); }
        catch
        {
            _autoEnableSentForCurrentShare = false;
            CancelSystemAudioTimeout();
            _systemAudioPending = false;
            _pendingSystemAudioMuted = !_confirmedSystemAudioEnabled;
            NotifyAll();
            throw;
        }
    }

    public void ApplyMicrophoneMuteConfirmation(MicrophoneMuteChanged confirmation)
    {
        CancelMicrophoneMuteTimeout();
        _confirmedMicrophoneMuted = confirmation.IsMuted;
        _pendingMicrophoneMuted = confirmation.IsMuted;
        _microphoneMutePending = false;
        NotifyAll();
    }

    public void ApplySystemAudioConfirmation(SystemAudioChanged confirmation)
    {
        _confirmedSystemAudioEnabled = confirmation.Enabled;
        // An initial/previous capture snapshot can arrive while activation or stop is pending.
        // Keep waiting for the requested state; a targeted backend error clears the lane.
        if (_systemAudioPending && confirmation.Enabled == _pendingSystemAudioMuted) { NotifyAll(); return; }
        CancelSystemAudioTimeout();
        _pendingSystemAudioMuted = !confirmation.Enabled;
        _systemAudioPending = false;
        NotifyAll();
    }

    public void ApplyRoomAudioMuteConfirmation(RoomAudioMuteChanged confirmation)
    {
        CancelRoomMuteTimeout();
        UntrackRoomCommand(RoomCommandLane.Mute);
        _confirmedRoomMuted = confirmation.IsMuted;
        _pendingRoomMuted = confirmation.IsMuted;
        _roomMutePending = false;
        NotifyAll();
    }

    public void ApplyRoomMixerConfirmation(RoomMixerChanged confirmation)
    {
        ValidateRoomMixer(confirmation.MasterPercent, confirmation.BassDb, confirmation.VoiceDb, confirmation.TrebleDb);
        ValidateRoomNoiseReduction(confirmation.NoiseReductionDb);
        CancelRoomMixerTimeout();
        UntrackRoomCommand(RoomCommandLane.Mixer);
        _confirmedRoomMasterPercent = confirmation.MasterPercent;
        _confirmedRoomBassDb = confirmation.BassDb;
        _confirmedRoomVoiceDb = confirmation.VoiceDb;
        _confirmedRoomTrebleDb = confirmation.TrebleDb;
        _confirmedRoomNoiseReductionDb = confirmation.NoiseReductionDb;
        _roomMixerPending = false;
        SnapRoomMixerToConfirmed();
        NotifyAll();
    }

    public void ApplyRoomMeter(RoomAudioMeterChanged meter)
    {
        ValidateMeter(meter);
        _roomPreLimiterPeakQ15 = meter.PreLimiterPeakQ15;
        _roomPostLimiterPeakQ15 = meter.PostLimiterPeakQ15;
        _roomLimiterGainReductionQ15 = meter.LimiterGainReductionQ15;
        _roomLimiterActive = meter.LimiterActive;
        RoomMeterChanged?.Invoke(this, EventArgs.Empty);
    }

    public void ApplyMediaError(MediaError error)
    {
        _lastErrorMessage = $"{error.Code}: {error.Detail}";
        _lastError = error;
        if (error.Code.StartsWith("system-audio-", StringComparison.Ordinal))
        {
            CancelSystemAudioTimeout();
            _systemAudioPending = false;
            _pendingSystemAudioMuted = !_confirmedSystemAudioEnabled;
        }
        if (error.Code.StartsWith("microphone-mute-", StringComparison.Ordinal) || error.Code.StartsWith("mic-mute-", StringComparison.Ordinal))
        {
            CancelMicrophoneMuteTimeout();
            _microphoneMutePending = false;
            _pendingMicrophoneMuted = _confirmedMicrophoneMuted;
        }
        if (error.Code.StartsWith("room-mute-", StringComparison.Ordinal))
        {
            FailRoomMuteLane();
        }
        else if (error.Code.StartsWith("room-mixer-", StringComparison.Ordinal))
        {
            var lane = FirstPendingRoomCommand();
            if (lane == RoomCommandLane.Mute) FailRoomMuteLane();
            else if (lane == RoomCommandLane.Mixer) FailRoomMixerLane();
        }
        NotifyAll();
    }

    private void FailRoomMuteLane()
    {
        CancelRoomMuteTimeout();
        UntrackRoomCommand(RoomCommandLane.Mute);
        if (_roomMutePending)
        {
            _roomMutePending = false;
            _pendingRoomMuted = _confirmedRoomMuted;
        }
    }

    private void FailRoomMixerLane()
    {
        CancelRoomMixerTimeout();
        UntrackRoomCommand(RoomCommandLane.Mixer);
        if (_roomMixerPending)
        {
            _roomMixerPending = false;
            SnapRoomMixerToConfirmed();
        }
    }

    public void SetPendingRoomVolume(int value)
    {
        ValidateRoom(value);
        _pendingRoomVolume = value;
        OnPropertyChanged(nameof(PendingRoomVolume));
        if (IsEnabled) _scheduler.Schedule(() => SendPendingAsync(CancellationToken.None));
    }

    public void SetPendingMicrophoneGain(int value)
    {
        ValidateMicrophone(value);
        _pendingMicrophoneGain = value;
        OnPropertyChanged(nameof(PendingMicrophoneGain));
        if (IsEnabled) _scheduler.Schedule(() => SendPendingAsync(CancellationToken.None));
    }

    public void ResetRoomVolume() { _pendingRoomVolume = 100; OnPropertyChanged(nameof(PendingRoomVolume)); }
    public void ResetMicrophoneGain() { _pendingMicrophoneGain = 100; OnPropertyChanged(nameof(PendingMicrophoneGain)); }

    public Task CommitRoomVolumeAsync(int value, CancellationToken cancellationToken)
    {
        ValidateRoom(value); _pendingRoomVolume = value; OnPropertyChanged(nameof(PendingRoomVolume)); _scheduler.Cancel();
        return SendPendingAsync(cancellationToken);
    }

    public Task CommitMicrophoneGainAsync(int value, CancellationToken cancellationToken)
    {
        ValidateMicrophone(value); _pendingMicrophoneGain = value; OnPropertyChanged(nameof(PendingMicrophoneGain)); _scheduler.Cancel();
        return SendPendingAsync(cancellationToken);
    }

    public Task ToggleNoiseIsolationAsync(CancellationToken cancellationToken)
    {
        _pendingNoiseIsolation = !_confirmedNoiseIsolation;
        OnPropertyChanged(nameof(PendingNoiseIsolation));
        _scheduler.Cancel();
        return SendPendingAsync(cancellationToken);
    }

    public void ApplyConfirmation(AudioControlsChanged confirmation)
    {
        ValidateRoom(confirmation.RoomVolume);
        ValidateMicrophone(confirmation.MicrophoneGain);
        _confirmedRoomVolume = confirmation.RoomVolume;
        _confirmedMicrophoneGain = confirmation.MicrophoneGain;
        _confirmedNoiseIsolation = confirmation.NoiseIsolation;
        _isPending = false;
        SnapToConfirmed();
        NotifyAll();
    }

    private async Task SendPendingAsync(CancellationToken cancellationToken)
    {
        if (!IsEnabled) throw new InvalidOperationException("Audio controls are not ready for this call.");
        _isPending = true;
        NotifyAll();
        try
        {
            await _engine.SendAsync(new SetAudioControls(_pendingRoomVolume, _pendingMicrophoneGain, _pendingNoiseIsolation), cancellationToken);
        }
        catch
        {
            _isPending = false;
            SnapToConfirmed();
            NotifyAll();
            throw;
        }
    }

    private async Task SendRoomMixerAsync(CancellationToken cancellationToken)
    {
        try
        {
            await _engine.SendAsync(new SetRoomMixer(
                _pendingRoomMasterPercent, _pendingRoomBassDb, _pendingRoomVoiceDb, _pendingRoomTrebleDb, _pendingRoomNoiseReductionDb), cancellationToken);
        }
        catch
        {
            CancelRoomMixerTimeout();
            UntrackRoomCommand(RoomCommandLane.Mixer);
            _roomMixerPending = false;
            SnapRoomMixerToConfirmed();
            NotifyAll();
            throw;
        }
    }

    private void SnapToConfirmed()
    {
        _pendingRoomVolume = _confirmedRoomVolume;
        _pendingMicrophoneGain = _confirmedMicrophoneGain;
        _pendingNoiseIsolation = _confirmedNoiseIsolation;
    }

    private void StartMicrophoneMuteTimeout()
    {
        CancelTimeout(ref _microphoneMuteTimeout, ref _microphoneMuteGeneration);
        var generation = _microphoneMuteGeneration;
        _microphoneMuteTimeout = _confirmationTimeouts.Schedule(ConfirmationTimeout, () =>
        {
            if (generation != _microphoneMuteGeneration || !_microphoneMutePending) return;
            _microphoneMuteTimeout = null;
            _microphoneMutePending = false;
            _pendingMicrophoneMuted = _confirmedMicrophoneMuted;
            SetTimeoutError("microphone-mute-confirmation-timeout");
            NotifyAll();
        });
    }

    private void StartSystemAudioTimeout()
    {
        CancelTimeout(ref _systemAudioTimeout, ref _systemAudioGeneration);
        var generation = _systemAudioGeneration;
        _systemAudioTimeout = _confirmationTimeouts.Schedule(TimeSpan.FromSeconds(6), () =>
        {
            if (generation != _systemAudioGeneration || !_systemAudioPending) return;
            _systemAudioTimeout = null;
            _systemAudioPending = false;
            _pendingSystemAudioMuted = !_confirmedSystemAudioEnabled;
            _autoEnableSentForCurrentShare = false;
            SetTimeoutError("system-audio-confirmation-timeout");
            NotifyAll();
        });
    }

    private void StartRoomMuteTimeout()
    {
        CancelTimeout(ref _roomMuteTimeout, ref _roomMuteGeneration);
        var generation = _roomMuteGeneration;
        _roomMuteTimeout = _confirmationTimeouts.Schedule(ConfirmationTimeout, () =>
        {
            if (generation != _roomMuteGeneration || !_roomMutePending) return;
            _roomMuteTimeout = null;
            _roomMutePending = false;
            _pendingRoomMuted = _confirmedRoomMuted;
            UntrackRoomCommand(RoomCommandLane.Mute);
            SetTimeoutError("room-mute-confirmation-timeout");
            NotifyAll();
        });
    }

    private void StartRoomMixerTimeout()
    {
        CancelTimeout(ref _roomMixerTimeout, ref _roomMixerGeneration);
        var generation = _roomMixerGeneration;
        _roomMixerTimeout = _confirmationTimeouts.Schedule(ConfirmationTimeout, () =>
        {
            if (generation != _roomMixerGeneration || !_roomMixerPending) return;
            _roomMixerTimeout = null;
            _roomMixerPending = false;
            SnapRoomMixerToConfirmed();
            UntrackRoomCommand(RoomCommandLane.Mixer);
            SetTimeoutError("room-mixer-confirmation-timeout");
            NotifyAll();
        });
    }

    private void CancelMicrophoneMuteTimeout() => CancelTimeout(ref _microphoneMuteTimeout, ref _microphoneMuteGeneration);
    private void CancelSystemAudioTimeout() => CancelTimeout(ref _systemAudioTimeout, ref _systemAudioGeneration);
    private void CancelRoomMuteTimeout() => CancelTimeout(ref _roomMuteTimeout, ref _roomMuteGeneration);
    private void CancelRoomMixerTimeout() => CancelTimeout(ref _roomMixerTimeout, ref _roomMixerGeneration);
    private static void CancelTimeout(ref IDisposable? timeout, ref long generation)
    {
        generation++;
        var current = timeout;
        timeout = null;
        current?.Dispose();
    }
    private void SetTimeoutError(string code)
    {
        _lastError = new MediaError(code, "native-confirmation-timeout");
        _lastErrorMessage = $"{code}: native-confirmation-timeout";
    }
    private void TrackRoomCommand(RoomCommandLane lane)
    {
        _pendingRoomCommands.Remove(lane);
        _pendingRoomCommands.Add(lane);
    }
    private void UntrackRoomCommand(RoomCommandLane lane) => _pendingRoomCommands.Remove(lane);
    private RoomCommandLane? FirstPendingRoomCommand()
    {
        _pendingRoomCommands.RemoveAll(lane => lane == RoomCommandLane.Mute ? !_roomMutePending : !_roomMixerPending);
        return _pendingRoomCommands.Count == 0 ? null : _pendingRoomCommands[0];
    }
    private void NotifyAll()
    {
        foreach (var property in new[]
        {
            nameof(IsEnabled), nameof(IsPending), nameof(ConfirmedRoomVolume), nameof(ConfirmedMicrophoneGain), nameof(ConfirmedNoiseIsolation),
            nameof(PendingRoomVolume), nameof(PendingMicrophoneGain), nameof(PendingNoiseIsolation), nameof(CanToggleMicrophoneMute),
            nameof(CanToggleSystemAudioMute), nameof(CanToggleRoomMute), nameof(CanOpenRoomMixer), nameof(ConfirmedMicrophoneMuted),
            nameof(CanOpenAudioMixer),
            nameof(PendingMicrophoneMuted), nameof(IsMicrophoneMutePending), nameof(ConfirmedSystemAudioEnabled), nameof(ConfirmedSystemAudioMuted),
            nameof(PendingSystemAudioMuted), nameof(IsSystemAudioPending), nameof(ConfirmedRoomMuted), nameof(PendingRoomMuted),
            nameof(IsRoomMutePending), nameof(IsRoomMixerPending), nameof(ConfirmedRoomMasterPercent), nameof(PendingRoomMasterPercent),
            nameof(ConfirmedRoomBassDb), nameof(PendingRoomBassDb), nameof(ConfirmedRoomVoiceDb), nameof(PendingRoomVoiceDb),
            nameof(ConfirmedRoomTrebleDb), nameof(PendingRoomTrebleDb), nameof(RoomPreLimiterPeakQ15), nameof(RoomPostLimiterPeakQ15),
            nameof(ConfirmedRoomNoiseReductionDb), nameof(PendingRoomNoiseReductionDb),
            nameof(RoomLimiterGainReductionQ15), nameof(IsLimiterWarningVisible), nameof(IsHighGainWarningVisible), nameof(LastErrorMessage), nameof(LastError)
        }) OnPropertyChanged(property);
    }
    private static void ValidateRoom(int value) { if (value is < 0 or > 100) throw new ArgumentOutOfRangeException(nameof(value)); }
    private static void ValidateMicrophone(int value) { if (value is < 0 or > 150) throw new ArgumentOutOfRangeException(nameof(value)); }
    private static void ValidateRoomNoiseReduction(int value) { if (value is < 0 or > 30) throw new ArgumentOutOfRangeException(nameof(value)); }
    private static void ValidateRoomMixer(int masterPercent, int bassDb, int voiceDb, int trebleDb)
    {
        if (masterPercent is < 0 or > 300 || bassDb is < -12 or > 12 || voiceDb is < -12 or > 12 || trebleDb is < -12 or > 12)
            throw new ArgumentOutOfRangeException(nameof(masterPercent));
    }
    private static void ValidateMeter(RoomAudioMeterChanged meter)
    {
        if (meter.PreLimiterPeakQ15 is < 0 or > 32767 || meter.PostLimiterPeakQ15 is < 0 or > 32767 ||
            meter.LimiterGainReductionQ15 is < 0 or > 32767) throw new ArgumentOutOfRangeException(nameof(meter));
    }
    private void SnapRoomMixerToConfirmed()
    {
        _pendingRoomMasterPercent = _confirmedRoomMasterPercent;
        _pendingRoomBassDb = _confirmedRoomBassDb;
        _pendingRoomVoiceDb = _confirmedRoomVoiceDb;
        _pendingRoomTrebleDb = _confirmedRoomTrebleDb;
        _pendingRoomNoiseReductionDb = _confirmedRoomNoiseReductionDb;
    }
    private void ResetIndependentAudio()
    {
        CancelMicrophoneMuteTimeout();
        CancelSystemAudioTimeout();
        CancelRoomMuteTimeout();
        CancelRoomMixerTimeout();
        _pendingRoomCommands.Clear();
        _microphoneMutePending = false;
        _systemAudioPending = false;
        _roomMutePending = false;
        _roomMixerPending = false;
        _confirmedMicrophoneMuted = false;
        _pendingMicrophoneMuted = false;
        _confirmedSystemAudioEnabled = false;
        _pendingSystemAudioMuted = false;
        _confirmedRoomMuted = false;
        _pendingRoomMuted = false;
        _confirmedRoomMasterPercent = 100;
        _confirmedRoomBassDb = _confirmedRoomVoiceDb = _confirmedRoomTrebleDb = 0;
        _confirmedRoomNoiseReductionDb = 0;
        SnapRoomMixerToConfirmed();
        _roomPreLimiterPeakQ15 = _roomPostLimiterPeakQ15 = _roomLimiterGainReductionQ15 = 0;
        _roomLimiterActive = false;
        _confirmedDesktopSharing = false;
        _manualSystemMuteForCurrentShare = false;
        _autoEnableSentForCurrentShare = false;
        _lastErrorMessage = null;
        _lastError = null;
    }
    private void OnPropertyChanged([CallerMemberName] string? name = null) => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
}
