using System.IO;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using TeleStudio.Contracts;
using TeleStudio.Presentation;

namespace TeleStudio.Desktop;

public enum SurfaceAcknowledgement { Opened, Released, Removed }

public class NativeEngineCommandSink : IEngineCommandSink, IAsyncDisposable
{
    private static readonly UTF8Encoding StrictUtf8 = new(false, true);
    private readonly IReadOnlyDictionary<CallProtocol, IProtocolConsole> _consoles;
    private readonly SemaphoreSlim _operationGate = new(1, 1);
    private readonly CancellationTokenSource _commands = new();
    private Task? _disposeTask;
    protected CancellationToken RetirementToken => _commands.Token;
    protected void ThrowIfDisposed() => ObjectDisposedException.ThrowIf(_disposed, this);
    protected static async Task WaitForCommandGateAsync(SemaphoreSlim gate, CancellationToken token)
    {
        if (!await gate.WaitAsync(ProtocolConsoleProcess.CommandTimeout, token))
            throw new TimeoutException("Native command gate did not become available within its deadline.");
    }
    private readonly object _stateLock = new();
    private readonly object _eventDispatchLock = new();
    private readonly Dictionary<ConsoleGeneration, long> _lastSequences = new();
    private readonly HashSet<ConsoleGeneration> _videoStartupReady = [];
    private readonly List<NativeJsonEvent> _preConnectedSnapshots = [];
    private MediaPreferenceSnapshot _preferences = new(CodecPreference.Auto, QualityMode.Auto, EndpointProfile.Auto);
    private NetworkConnectionMode _networkMode = NetworkConnectionMode.Building;
    private ConnectionTuning? _connectionTuning;
    private DialPreflight? _dialPreflight;
    private IProtocolConsole? _idlePreflightConsole;
    private long _idlePreflightGeneration;
    private IProtocolConsole? _activeConsole;
    private CallProtocol? _activeProtocol;
    private long _activeProcessGeneration;
    private long _callGeneration;
    private long _lastTerminalCallGeneration = -1;
    private long _recordingOperationGeneration;
    private string? _activeRemoteAddress;
    private bool _connected;
    private MediaCapabilitiesChanged _capabilities = EmptyCapabilities;
    private VideoSource _confirmedSource = VideoSource.Camera;
    private bool _confirmedMuted;
    private bool _confirmedSystemAudioEnabled;
    private bool _confirmedRoomAudioMuted;
    private RoomMixerChanged _confirmedRoomMixer = DefaultRoomMixer;
    private AudioControlsChanged _confirmedAudio = new(100, 100, true);
    private RecordingStateChanged _confirmedRecording = IdleRecording;
    private bool _recordingInitialSnapshotSeen;
    private bool _recordStartRequested;
    private bool _recordStopRequested;
    private bool? _recordPauseRequested;
    private bool _awaitingFailedState;
    private bool _recordingStreamInvalid;
    private bool _requestedRecordingMicrophone;
    private bool _requestedRecordingRoom;
    private string? _requestedDesktopId;
    private string? _confirmedDesktopId;
    private PendingRecordingFinalization? _pendingFinalization;
    private string? _historicalRecordingFileName;
    private readonly Dictionary<ulong, SurfaceOwner> _surfaceAcknowledgements = new();
    private readonly Dictionary<ulong, SurfaceOwner> _localSurfaceAcknowledgements = new();
    private static long _nextPresentationEpoch;
    private ulong _videoRevision, _lastRemoteNativeEpoch, _lastLocalNativeEpoch;
    private SetVideoPreferences? _videoPreferences;
    private sealed record SurfaceOwner(IProtocolConsole Console, long Generation, ulong NativeEpoch, long CallGeneration);
    private bool _started;
    private bool _disposed;
    private readonly TimeProvider _timeProvider;
    private readonly DeviceTestDeadline? _deviceTestDeadline;
    private ITimer? _deviceTestTimer;
    private PendingSipDial? _pendingSipDial;
    private IProtocolConsole? _preparingConsole;
    private IProtocolConsole? _retiringConsole;
    private bool _retiringCallTerminated;
    private readonly bool _sharedWorkerLifecycle;
    public SipEngineKind SelectedSipEngine { get; protected set; } = SipEngineKind.Opal;
    private bool _held;
    public bool ConfirmedHeld { get { lock (_stateLock) return _connected && _held; } }
    protected bool HasActiveOrPendingCall { get { lock (_stateLock) return _activeConsole is not null; } }
    public bool CanHold { get { lock (_stateLock) return _connected && (_sharedWorkerLifecycle || _activeProtocol == CallProtocol.Sip) && _capabilities.HoldControl; } }
    public bool CanSendDtmf { get { lock (_stateLock) return _connected && (_sharedWorkerLifecycle || _activeProtocol == CallProtocol.Sip) && _capabilities.DtmfControl; } }

    // Scope all required settings to one dial. A later success cannot erase an earlier rejection.
    private sealed class DialPreflight(ConnectionTuning settings)
    {
        public ConnectionTuning Settings { get; } = settings;
        public Exception? Failure { get; private set; }
        public TaskCompletionSource? AudioAcknowledgement { get; set; }
        public TaskCompletionSource? VideoAcknowledgement { get; set; }
        public void Fail(Exception error)
        {
            Failure ??= error;
            AudioAcknowledgement?.TrySetException(Failure);
            VideoAcknowledgement?.TrySetException(Failure);
        }
        public void ThrowIfFailed() { if (Failure is { } error) throw error; }
    }

    private sealed class PendingSipDial(IProtocolConsole console, long generation) : IDisposable
    {
        public IProtocolConsole Console { get; } = console;
        public long Generation { get; } = generation;
        public CancellationTokenSource Cancellation { get; } = new();
        public ITimer? Timer { get; set; }
        public bool Expired { get; set; }
        public void Dispose() { Timer?.Dispose(); Cancellation.Dispose(); }
    }

    internal NativeEngineCommandSink(IProtocolConsole h323, IProtocolConsole sip, TimeProvider? timeProvider = null, DateTimeOffset? deviceTestCutoff = null, bool sharedWorkerLifecycle = false)
    {
        ArgumentNullException.ThrowIfNull(h323);
        ArgumentNullException.ThrowIfNull(sip);
        _timeProvider = timeProvider ?? TimeProvider.System;
        _sharedWorkerLifecycle = sharedWorkerLifecycle;
        _deviceTestDeadline = deviceTestCutoff is { } cutoff ? new(cutoff) : DeviceTestDeadline.FromEnvironment();
        if (h323.Protocol != CallProtocol.H323 || sip.Protocol != CallProtocol.Sip)
        {
            throw new ArgumentException("The native consoles must match their protocol slots.");
        }
        _consoles = new Dictionary<CallProtocol, IProtocolConsole>
        {
            [CallProtocol.H323] = h323,
            [CallProtocol.Sip] = sip
        };
        foreach (var console in _consoles.Values)
        {
            console.LineReceived += Console_LineReceived;
            console.GenerationEnded += Console_GenerationEnded;
        }
        if (_deviceTestDeadline is not null)
            _deviceTestTimer = _timeProvider.CreateTimer(_ => _ = EnforceDeviceTestDeadlineAsync(), null, TimeSpan.FromSeconds(1), TimeSpan.FromSeconds(1));
    }

    private async Task EnforceDeviceTestDeadlineAsync()
    {
        try
        {
            bool end;
            lock (_stateLock) end = !_disposed && _activeConsole is not null && _deviceTestDeadline?.EndCall(_activeRemoteAddress, _timeProvider.GetUtcNow()) == true;
            if (end) await SendAsync(new HangUp(), CancellationToken.None);
        }
        catch (ObjectDisposedException) { }
        catch (Exception error) { TeleStudio.Desktop.Runtime.DiagnosticLog.Write("device-test-deadline", error.Message); }
    }

    public event EventHandler<EngineEvent>? EventReceived;

    // Called before StartAsync: preferences must exist before either listener starts.
    public void ConfigureVideoStartup(SetVideoPreferences preferences)
    {
        ArgumentNullException.ThrowIfNull(preferences);
        if (_started) throw new InvalidOperationException("Use SetVideoPreferences after startup.");
        _videoPreferences = preferences;
        foreach (var console in _consoles.Values) console.ConfigureVideoStartup(preferences);
    }

    public VideoSource ConfirmedSource { get { lock (_stateLock) return _confirmedSource; } }
    public bool ConfirmedMuted { get { lock (_stateLock) return _confirmedMuted; } }
    public bool ConfirmedMicrophoneMuted { get { lock (_stateLock) return _confirmedMuted; } }
    public bool ConfirmedSystemAudioEnabled { get { lock (_stateLock) return _confirmedSystemAudioEnabled; } }
    public bool ConfirmedRoomAudioMuted { get { lock (_stateLock) return _confirmedRoomAudioMuted; } }
    public RoomMixerChanged ConfirmedRoomMixer { get { lock (_stateLock) return _confirmedRoomMixer; } }
    public AudioControlsChanged ConfirmedAudioControls { get { lock (_stateLock) return _confirmedAudio; } }
    public RecordingStateChanged ConfirmedRecordingState { get { lock (_stateLock) return _confirmedRecording; } }
    public string? ConfirmedSharedDesktopId { get { lock (_stateLock) return _confirmedDesktopId; } }
    public string? HistoricalRecordingFileName { get { lock (_stateLock) return _historicalRecordingFileName; } }

    public async Task ActivateProtocolAsync(CallProtocol protocol, CancellationToken token)
    {
        using var commandLifetime = CancellationTokenSource.CreateLinkedTokenSource(token, RetirementToken);
        token = commandLifetime.Token;
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (!Enum.IsDefined(protocol)) throw new ArgumentOutOfRangeException(nameof(protocol));
        await WaitForCommandGateAsync(_operationGate, token);
        try
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            if (_activeConsole is not null) throw new InvalidOperationException("End the current call before changing protocol.");
        }
        finally { _operationGate.Release(); }
    }
    public async Task ApplyVideoNetworkLimitAsync(int fps, int bitrate, bool text, CancellationToken token)
    {
        using var commandLifetime = CancellationTokenSource.CreateLinkedTokenSource(token, RetirementToken);
        token = commandLifetime.Token;
        if (fps is < 2 or > 30 || bitrate is < 128 or > 20000) throw new ArgumentOutOfRangeException(nameof(fps));
        await WaitForCommandGateAsync(_operationGate, token);
        try { await WriteToActiveAsync($"network-video {fps} {bitrate} {(text ? "text" : "motion")}", token); }
        finally { _operationGate.Release(); }
    }

    internal long? ConnectedCallGeneration { get { lock (_stateLock) return _connected ? _callGeneration : null; } }

    internal async Task<bool> ApplyAutomaticVideoTuningAsync(long generation, SetVideoPreferences? preferences,
        int fps, int bitrate, bool text, CancellationToken token)
    {
        using var commandLifetime = CancellationTokenSource.CreateLinkedTokenSource(token, RetirementToken);
        token = commandLifetime.Token;
        if (fps is < 2 or > 30 || bitrate is < 128 or > 20000) throw new ArgumentOutOfRangeException(nameof(fps));
        await WaitForCommandGateAsync(_operationGate, token);
        try
        {
            IProtocolConsole console;
            lock (_stateLock)
            {
                if (_disposed || !_connected || _callGeneration != generation || _activeConsole is null || _retiringConsole is not null) return false;
                console = _activeConsole;
            }
            if (preferences is not null) await console.WriteLineAsync(VideoCommandCodec.Format(preferences), token);
            // Hold the same command gate across both writes; hangup cannot retire the child between them.
            lock (_stateLock)
                if (!_connected || _callGeneration != generation || !ReferenceEquals(_activeConsole, console)) return false;
            await console.WriteLineAsync($"network-video {fps} {bitrate} {(text ? "text" : "motion")}", token);
            if (preferences is not null)
            {
                _videoPreferences = preferences;
                foreach (var target in _consoles.Values) target.ConfigureVideoStartup(preferences);
            }
            return true;
        }
        catch (Exception error) when (error is IOException or InvalidOperationException or OperationCanceledException or System.ComponentModel.Win32Exception && HasEndedCallGeneration(generation))
        {
            // Remote teardown may close the pipe during an automatic write. Only a terminal
            // event for this exact call makes that work obsolete; genuine current-call errors propagate.
            return false;
        }
        finally { _operationGate.Release(); }
    }

    private bool HasEndedCallGeneration(long generation)
    {
        lock (_stateLock) return _lastTerminalCallGeneration == generation &&
            (!_connected || _callGeneration != generation || _activeConsole is null);
    }

    public async Task StartAsync(CancellationToken cancellationToken)
    {
        using var commandLifetime = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken, RetirementToken);
        cancellationToken = commandLifetime.Token;
        ObjectDisposedException.ThrowIf(_disposed, this);
        await WaitForCommandGateAsync(_operationGate, cancellationToken);
        try
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            if (_started)
            {
                return;
            }
            await Task.WhenAll(_consoles.Values.Select(console => console.StartAsync(cancellationToken)));
            _started = true;
        }
        finally
        {
            _operationGate.Release();
        }
    }

    public virtual async Task SendAsync(EngineCommand command, CancellationToken cancellationToken)
    {
        using var commandLifetime = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken, RetirementToken);
        cancellationToken = commandLifetime.Token;
        ArgumentNullException.ThrowIfNull(command);
        ObjectDisposedException.ThrowIf(_disposed, this);
        await WaitForCommandGateAsync(_operationGate, cancellationToken);
        try
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            if (!_started)
            {
                throw new InvalidOperationException("Native call engines are not ready.");
            }
            switch (command)
            {
                case SetConnectionTuning tuning:
                    if (_activeConsole is not null && tuning.Settings.AudioCommand != _connectionTuning?.AudioCommand) throw new InvalidOperationException("Audio settings require reconnect.");
                    _connectionTuning = tuning.Settings;
                    break;
                case SetCallHeld held:
                    await RequireConnectedCapability(c => c.HoldControl, "hold/resume").WriteLineAsync(held.Held ? "hold" : "resume", cancellationToken);
                    break;
                case SendDtmf dtmf:
                    await RequireConnectedCapability(c => c.DtmfControl, "DTMF").WriteLineAsync("dtmf " + dtmf.Digits, cancellationToken);
                    break;
                case Dial dial:
                    if (_deviceTestDeadline?.BlockStart(dial.Destination, _timeProvider.GetUtcNow()) == true)
                        throw new InvalidOperationException("Physical device test deadline reached. Only localhost calls remain allowed.");
                    await DialAsync(dial, cancellationToken);
                    break;
                case Answer:
                    if (_deviceTestDeadline?.BlockStart(_activeRemoteAddress, _timeProvider.GetUtcNow()) == true)
                        throw new InvalidOperationException("Physical device test deadline reached. Incoming physical calls cannot be answered.");
                    ClearLiveMediaAtCallStart();
                    if (_videoPreferences is not null)
                        await WriteToActiveAsync(VideoCommandCodec.Format(_videoPreferences), cancellationToken);
                    await WriteToActiveAsync("answer", cancellationToken);
                    break;
                case Reject:
                    await WriteToActiveAsync("reject", cancellationToken);
                    break;
                case HangUp:
                    PendingSipDial? pending;
                    IProtocolConsole selectedForHangup;
                    bool retireSelected;
                    lock (_stateLock)
                    {
                        if (_activeConsole is null) break; // A queued second click after successful teardown is already satisfied.
                        selectedForHangup = _activeConsole;
                        pending = _pendingSipDial;
                        retireSelected = !_sharedWorkerLifecycle && (_activeProtocol == CallProtocol.Sip);
                    }
                    if (pending is not null)
                        await RetirePendingSipDialAsync(pending, new CallEnded("cancelled"), cancellationToken);
                    else if (retireSelected)
                        await RetireActiveSipCallAsync(cancellationToken);
                    else
                        await selectedForHangup.WriteLineAsync("hangup", cancellationToken);
                    break;
                case SetMediaPreferences preferences:
                    await SetMediaPreferencesAsync(preferences, cancellationToken);
                    break;
                case SetNetworkMode networkMode:
                    await SetNetworkModeAsync(networkMode, cancellationToken);
                    break;
                case SetVideoPreferences video:
                    var videoText = VideoCommandCodec.Format(video);
                    if (_activeConsole is not null)
                        await RequireConnectedCapability(c => c.VideoControl, "video control").WriteLineAsync(videoText, cancellationToken);
                    List<IProtocolConsole> idleTargets;
                    lock (_stateLock)
                    {
                        idleTargets = _consoles.Values.Where(console => !ReferenceEquals(console, _activeConsole) &&
                            _videoStartupReady.Contains(new ConsoleGeneration(console, console.ProcessGeneration))).ToList();
                    }
                    foreach (var console in idleTargets) await console.WriteLineAsync(videoText, cancellationToken);
                    _videoPreferences = video;
                    foreach (var console in _consoles.Values) console.ConfigureVideoStartup(video);
                    break;
                case SetCameraEnabled camera:
                    await RequireVideoConsole(camera.Enabled).WriteLineAsync(VideoCommandCodec.Format(camera), cancellationToken);
                    break;
                case SetDesktopEnabled desktop:
                    await RequireVideoConsole(desktop.Enabled).WriteLineAsync(VideoCommandCodec.Format(desktop), cancellationToken);
                    break;
                case SetCameraOverlay overlay:
                    await RequireConnectedCapability(c => c.VideoControl, "video control").WriteLineAsync(VideoCommandCodec.Format(overlay), cancellationToken);
                    break;
                case SwitchSource source:
                    await SwitchSourceAsync(source, cancellationToken);
                    break;
                case SetMuted mute:
                    await SetMutedAsync(mute, cancellationToken);
                    break;
                case SetAudioControls audio:
                    await SetAudioControlsAsync(audio, cancellationToken);
                    break;
                case SetMicrophoneMuted microphone:
                    await SetMicrophoneMutedAsync(microphone, cancellationToken);
                    break;
                case SetSystemAudioEnabled systemAudio:
                    await SetSystemAudioEnabledAsync(systemAudio, cancellationToken);
                    break;
                case SetRoomAudioMuted roomAudio:
                    await SetRoomAudioMutedAsync(roomAudio, cancellationToken);
                    break;
                case SetRoomMixer roomMixer:
                    await SetRoomMixerAsync(roomMixer, cancellationToken);
                    break;
                case StartLectureRecording recording:
                    await StartRecordingAsync(recording, cancellationToken);
                    break;
                case SetLectureRecordingPaused pause:
                    await SetRecordingPausedAsync(pause.Paused, cancellationToken);
                    break;
                case StopLectureRecording:
                    await StopRecordingAsync(cancellationToken);
                    break;
                default:
                    throw new ArgumentOutOfRangeException(nameof(command));
            }
        }
        finally
        {
            _operationGate.Release();
        }
    }

    public async Task SendSurfaceAcknowledgementAsync(
        SurfaceAcknowledgement acknowledgement,
        ulong epoch,
        CancellationToken cancellationToken)
        => await SendOwnedSurfaceAcknowledgementAsync(false, acknowledgement, epoch, cancellationToken);

    public Task SendLocalSurfaceAcknowledgementAsync(SurfaceAcknowledgement acknowledgement, ulong epoch,
        CancellationToken cancellationToken) => SendOwnedSurfaceAcknowledgementAsync(true, acknowledgement, epoch, cancellationToken);

    private async Task SendOwnedSurfaceAcknowledgementAsync(bool local, SurfaceAcknowledgement acknowledgement,
        ulong epoch, CancellationToken cancellationToken)
    {
        ThrowIfDisposed();
        using var commandLifetime = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken, RetirementToken);
        cancellationToken = commandLifetime.Token;
        if (epoch == 0)
        {
            throw new ArgumentOutOfRangeException(nameof(epoch));
        }
        await WaitForCommandGateAsync(_operationGate, cancellationToken);
        try
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            var token = acknowledgement switch
            {
                SurfaceAcknowledgement.Opened => "surface-opened",
                SurfaceAcknowledgement.Released => "surface-released",
                SurfaceAcknowledgement.Removed => "surface-removed",
                _ => throw new ArgumentOutOfRangeException(nameof(acknowledgement))
            };
            if (local) token = "local-" + token;
            SurfaceOwner owner;
            var owners = local ? _localSurfaceAcknowledgements : _surfaceAcknowledgements;
            lock (_stateLock)
            {
                if (!owners.TryGetValue(epoch, out owner!) || owner.Console.ProcessGeneration != owner.Generation ||
                    (acknowledgement == SurfaceAcknowledgement.Opened && (!_connected || owner.CallGeneration != _callGeneration)))
                {
                    throw new InvalidOperationException("The remote surface epoch is no longer current.");
                }
                // A restarted native epoch in the same process/call slot must not receive an old release.
                if (owners.Any(pair => pair.Key != epoch && pair.Value.Console == owner.Console &&
                    pair.Value.Generation == owner.Generation && pair.Value.NativeEpoch == owner.NativeEpoch &&
                    pair.Value.CallGeneration > owner.CallGeneration))
                {
                    owners.Remove(epoch);
                    throw new InvalidOperationException("The surface owner has been superseded.");
                }
            }
            await owner.Console.WriteLineAsync($"{token} {owner.NativeEpoch}", cancellationToken);
            if (acknowledgement is SurfaceAcknowledgement.Released or SurfaceAcknowledgement.Removed)
            {
                lock (_stateLock) owners.Remove(epoch);
            }
        }
        finally
        {
            _operationGate.Release();
        }
    }

    public ValueTask DisposeAsync()
    {
        lock (_stateLock)
        {
            if (_disposeTask is null || _disposeTask.IsFaulted) _disposeTask = DisposeCoreAsync();
            return new(_disposeTask);
        }
    }

    private async Task DisposeCoreAsync()
    {
        _disposed = true;
        _commands.Cancel();
        _deviceTestTimer?.Dispose();
        CancellationTokenSource? pendingCancellation;
        lock (_stateLock) pendingCancellation = _pendingSipDial?.Cancellation;
        try { pendingCancellation?.Cancel(); } catch (ObjectDisposedException) { }
        // A dial-deadline retirement may already own this gate while waiting for
        // the official BYE transaction. Give that cleanup its full OPAL budget.
        if (!await _operationGate.WaitAsync(OpalCallLifecycle.TerminationTimeout))
            throw new TimeoutException("Native cleanup did not retire within its termination deadline; replacement was stopped.");
        try
        {
            foreach (var console in _consoles.Values)
            {
                console.LineReceived -= Console_LineReceived;
                console.GenerationEnded -= Console_GenerationEnded;
            }
            lock (_stateLock)
            {
                _pendingFinalization = null;
                _surfaceAcknowledgements.Clear();
                _localSurfaceAcknowledgements.Clear();
                ResetActiveCallLocked();
            }
            var shutdownErrors = new List<Exception>();
            foreach (var console in _consoles.Values)
            {
                try { await console.DisposeAsync(); }
                catch (Exception error) { shutdownErrors.Add(error); }
            }
            if (shutdownErrors.Count > 0)
                throw new AggregateException("One or more native engines could not confirm graceful shutdown.", shutdownErrors);
        }
        finally { _operationGate.Release(); }
        // Queued senders/deadlines must still release after observing _disposed.
        // No wait handle is allocated; GC can reclaim the semaphore safely.
    }

    private async Task DialAsync(Dial dial, CancellationToken cancellationToken)
    {
        ValidateDestination(dial.Destination);
        lock (_stateLock) if (_activeConsole is not null) throw new InvalidOperationException("A call is already active or pending.");
        var identity = LecturerNameCodec.Encode(dial.LecturerName);
        MediaPreferenceSnapshot preferences;
        NetworkConnectionMode networkMode;
        IProtocolConsole console;
        long callGeneration;
        PendingSipDial? pending = null;
        lock (_stateLock)
        {
            if (_activeConsole is not null)
            {
                throw new InvalidOperationException("A call is already active or pending.");
            }
            preferences = _preferences;
            networkMode = _networkMode;
            ValidateCodec(dial.Protocol, preferences.Codec, preferences.Endpoint);
            console = _consoles[dial.Protocol];
            callGeneration = checked(++_callGeneration);
            _activeConsole = console;
            _activeProtocol = dial.Protocol;
            _activeProcessGeneration = console.ProcessGeneration;
            _activeRemoteAddress = dial.Destination;
            ClearLiveMediaLocked();
            if (dial.Protocol == CallProtocol.Sip)
            {
                _preparingConsole = console;
                _activeProcessGeneration = long.MinValue;
                pending = new PendingSipDial(console, callGeneration);
                _pendingSipDial = pending;
                pending.Timer = _timeProvider.CreateTimer(state => _ = ExpireSipDialAsync((PendingSipDial)state!),
                    pending, TimeSpan.FromSeconds(30), Timeout.InfiniteTimeSpan);
            }
        }
        using var writes = pending is null ? null : CancellationTokenSource.CreateLinkedTokenSource(
            cancellationToken, pending.Cancellation.Token);
        var writeToken = writes?.Token ?? cancellationToken;
        DialPreflight? preflight = null;
        try
        {
            if (pending is not null)
            {
                await console.PrepareDialAsync(dial.Destination, writeToken).WaitAsync(writeToken);
                lock (_stateLock)
                {
                    _preparingConsole = null;
                    _activeProcessGeneration = console.ProcessGeneration;
                }
            }
            lock (_stateLock)
                if (_connectionTuning is { } settings) _dialPreflight = preflight = new DialPreflight(settings);
            if (_videoPreferences is not null)
                await console.WriteLineAsync(VideoCommandCodec.Format(_videoPreferences), writeToken).WaitAsync(writeToken);
            await WriteTransactionAsync(console,
                [NetworkModeCommand(networkMode), CodecCommand(preferences.Codec), QualityCommand(preferences.Quality), EndpointCommand(preferences.Endpoint, dial.Protocol), $"identity {identity}"],
                writeToken).WaitAsync(writeToken);
            if (preflight is not null)
            {
                await WriteConnectionPreflightAsync(console, preflight, writeToken);
            }
            lock (_stateLock) preflight?.ThrowIfFailed();
            await console.WriteLineAsync($"call {dial.Destination}", writeToken).WaitAsync(writeToken);
        }
        catch (OperationCanceledException) when (pending is { Expired: true } && !cancellationToken.IsCancellationRequested)
        {
            // The deadline task owns cleanup and its specific user-facing failure.
        }
        catch
        {
            if (pending is not null)
                await RetirePendingSipDialAsync(pending, null);
            lock (_stateLock)
            {
                if (!_connected && _callGeneration == callGeneration && ReferenceEquals(_activeConsole, console))
                {
                    ResetActiveCallLocked();
                }
            }
            throw;
        }
        finally
        {
            lock (_stateLock)
            {
                if (ReferenceEquals(_dialPreflight, preflight)) _dialPreflight = null;
                if (preflight is not null)
                {
                    preflight.AudioAcknowledgement = null;
                    preflight.VideoAcknowledgement = null;
                }
            }
        }
    }

    protected async Task ConfirmIdleConnectionTuningAsync(CallProtocol protocol, ConnectionTuning settings, CancellationToken token)
    {
        await WaitForCommandGateAsync(_operationGate, token);
        DialPreflight? preflight = null;
        try
        {
            IProtocolConsole console;
            lock (_stateLock)
            {
                if (_activeConsole is not null) throw new InvalidOperationException("Connection settings require an idle call engine.");
                console = _consoles[protocol];
                _idlePreflightConsole = console;
                _idlePreflightGeneration = console.ProcessGeneration;
                _dialPreflight = preflight = new DialPreflight(settings);
            }
            await WriteConnectionPreflightAsync(console, preflight, token);
        }
        finally
        {
            lock (_stateLock)
            {
                if (ReferenceEquals(_dialPreflight, preflight)) _dialPreflight = null;
                _idlePreflightConsole = null;
            }
            _operationGate.Release();
        }
    }

    private async Task WriteConnectionPreflightAsync(IProtocolConsole console, DialPreflight preflight, CancellationToken token)
    {
        var tuning = preflight.Settings;
        using var acknowledgementTimeout = CancellationTokenSource.CreateLinkedTokenSource(token);
        acknowledgementTimeout.CancelAfter(TimeSpan.FromSeconds(5));
        var acknowledgementToken = acknowledgementTimeout.Token;
        try
        {
            var audio = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            lock (_stateLock) { preflight.ThrowIfFailed(); preflight.AudioAcknowledgement = audio; }
            await console.WriteLineAsync(tuning.AudioCommand, acknowledgementToken).WaitAsync(acknowledgementToken);
            await audio.Task.WaitAsync(acknowledgementToken);
            var video = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            lock (_stateLock) { preflight.ThrowIfFailed(); preflight.VideoAcknowledgement = video; }
            await console.WriteLineAsync(tuning.VideoCommand(false), acknowledgementToken).WaitAsync(acknowledgementToken);
            await video.Task.WaitAsync(acknowledgementToken);
            lock (_stateLock) preflight.ThrowIfFailed();
        }
        catch (OperationCanceledException) when (!token.IsCancellationRequested)
        { throw new TimeoutException("The selected engine did not confirm the requested audio/video settings within five seconds."); }
    }

    private async Task ExpireSipDialAsync(PendingSipDial pending)
    {
        lock (_stateLock)
        {
            if (!ReferenceEquals(_pendingSipDial, pending) || _connected || _disposed) return;
            pending.Expired = true;
        }
        try { pending.Cancellation.Cancel(); } catch (ObjectDisposedException) { return; }
        try
        {
            await WaitForCommandGateAsync(_operationGate, RetirementToken);
            try { await RetirePendingSipDialAsync(pending, new CallFailed("sip-call-timeout")); }
            finally { _operationGate.Release(); }
        }
        catch (ObjectDisposedException) when (_disposed) { }
        catch (Exception error)
        {
            // Failed shared-worker cleanup keeps the call reserved until its
            // terminal event or process exit; do not invent an idle state.
            Runtime.StartupErrorLog.TryWriteOperation(error, "Pending-call-deadline/SIP", out _);
        }
    }

    // Caller owns the command gate. Retire before making another call possible,
    // so late responses to the timed-out INVITE cannot attach to the next call.
    private async Task RetirePendingSipDialAsync(PendingSipDial pending, EngineEvent? terminal, CancellationToken cancellationToken = default)
    {
        lock (_stateLock)
        {
            if (!ReferenceEquals(_pendingSipDial, pending) || _connected || _disposed) return;
            _retiringConsole = pending.Console;
            _retiringCallTerminated = false;
            _preparingConsole = null;
            _activeProcessGeneration = long.MinValue;
            StopSipDialDeadlineLocked();
        }
        Exception? cleanupError = null;
        var requestedTerminal = terminal;
        try
        {
            using var cleanup = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            cleanup.CancelAfter(_sharedWorkerLifecycle ? OpalCallLifecycle.TerminationTimeout : TimeSpan.FromSeconds(5));
            await pending.Console.AbortDialAsync(cleanup.Token).WaitAsync(cleanup.Token);
        }
        catch (Exception error)
        {
            // Preserve an actionable terminal state even if native teardown fails.
            Runtime.StartupErrorLog.TryWriteOperation(error, "Pending-call-teardown/SIP", out _);
            cleanupError = error;
            terminal = new CallFailed("sip-call-cleanup-failed");
        }
        lock (_eventDispatchLock)
        {
            lock (_stateLock)
            {
                _retiringConsole = null;
                if (_callGeneration != pending.Generation || _disposed) return;
                if (_sharedWorkerLifecycle && cleanupError is not null && !_retiringCallTerminated && ReferenceEquals(_activeConsole, pending.Console))
                {
                    _activeProcessGeneration = pending.Console.ProcessGeneration;
                    System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(cleanupError).Throw();
                }
                if (_sharedWorkerLifecycle && _retiringCallTerminated && cleanupError is OperationCanceledException)
                    terminal = requestedTerminal;
                ResetActiveCallLocked();
            }
            if (terminal is not null) EventReceived?.Invoke(this, terminal);
        }
        cancellationToken.ThrowIfCancellationRequested();
    }

    // A SIP media command can leave the native command loop unable to process a
    // later hangup. Retiring the routed child guarantees that media sockets are
    // closed and that a fresh engine is available for the next call.
    private async Task RetireActiveSipCallAsync(CancellationToken cancellationToken)
    {
        IProtocolConsole console;
        long callGeneration;
        lock (_stateLock)
        {
            if (_activeConsole is null) return; // The peer can end the call after HangUp selects this branch.
            console = _activeConsole;
            callGeneration = _callGeneration;
            _retiringConsole = console;
        }

        EngineEvent terminal = new CallEnded("local-hangup");
        try
        {
            using var cleanup = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            cleanup.CancelAfter(TimeSpan.FromSeconds(5));
            await console.AbortDialAsync(cleanup.Token).WaitAsync(cleanup.Token);
        }
        catch (Exception error)
        {
            Runtime.StartupErrorLog.TryWriteOperation(error, "HangUp/" + console.Protocol, out _);
            terminal = new CallFailed(console.Protocol == CallProtocol.Sip ? "sip-call-cleanup-failed" : "h323-call-cleanup-failed");
        }
        finally { lock (_stateLock) _retiringConsole = null; }

        lock (_eventDispatchLock)
        {
            lock (_stateLock)
            {
                if (_disposed || _lastTerminalCallGeneration == callGeneration) return;
                if (_callGeneration == callGeneration) ResetActiveCallLocked();
                _lastTerminalCallGeneration = callGeneration;
            }
            EventReceived?.Invoke(this, terminal);
        }
    }

    private void StopSipDialDeadlineLocked()
    {
        var pending = _pendingSipDial;
        _pendingSipDial = null;
        pending?.Dispose();
    }

    private async Task SetMediaPreferencesAsync(SetMediaPreferences command, CancellationToken cancellationToken)
    {
        var candidate = new MediaPreferenceSnapshot(command.Codec, command.Quality, command.Endpoint);
        ValidateEnums(candidate);
        IProtocolConsole? console;
        CallProtocol? protocol;
        lock (_stateLock)
        {
            console = _activeConsole;
            protocol = _activeProtocol;
        }
        if (protocol is not null)
        {
            ValidateCodec(protocol.Value, candidate.Codec, candidate.Endpoint);
            await WriteTransactionAsync(console!,
                [CodecCommand(candidate.Codec), QualityCommand(candidate.Quality), EndpointCommand(candidate.Endpoint, protocol.Value)], cancellationToken);
        }
        else if (candidate.Codec == CodecPreference.H265 && IsHardwareH264Only(candidate.Endpoint))
        {
            throw new NotSupportedException("The selected endpoint profile is H.264-only.");
        }
        lock (_stateLock)
        {
            _preferences = candidate;
        }
    }

    private async Task SetNetworkModeAsync(SetNetworkMode command, CancellationToken cancellationToken)
    {
        if (!Enum.IsDefined(command.Mode)) throw new ArgumentOutOfRangeException(nameof(command));
        lock (_stateLock)
        {
            if (_activeConsole is not null)
                throw new InvalidOperationException("Network mode cannot change during a call.");
            _networkMode = command.Mode;
        }
        var text = NetworkModeCommand(command.Mode);
        foreach (var console in _consoles.Values)
            await console.WriteLineAsync(text, cancellationToken);
    }

    private async Task SwitchSourceAsync(SwitchSource command, CancellationToken cancellationToken)
    {
        var console = RequireConnectedCapability(capabilities => capabilities.SourceControl, "source control");
        if (command.Source == VideoSource.Camera)
        {
            if (command.DisplayId is not null)
            {
                throw new ArgumentException("Camera source does not accept an identifier.", nameof(command));
            }
            await console.WriteLineAsync("source camera", cancellationToken);
            return;
        }
        if (command.Source != VideoSource.Desktop)
        {
            throw new ArgumentOutOfRangeException(nameof(command));
        }
        var encoded = EncodeOpaqueId(command.DisplayId, nameof(command.DisplayId));
        lock (_stateLock)
        {
            _requestedDesktopId = command.DisplayId;
        }
        try
        {
            await console.WriteLineAsync($"source desktop {encoded}", cancellationToken);
        }
        catch
        {
            lock (_stateLock)
            {
                if (string.Equals(_requestedDesktopId, command.DisplayId, StringComparison.Ordinal))
                {
                    _requestedDesktopId = null;
                }
            }
            throw;
        }
    }

    private async Task SetMutedAsync(SetMuted command, CancellationToken cancellationToken)
    {
        var console = RequireConnectedCapability(capabilities => capabilities.MuteControl, "mute control");
        await console.WriteLineAsync(command.IsMuted ? "mute on" : "mute off", cancellationToken);
    }

    private async Task SetAudioControlsAsync(SetAudioControls command, CancellationToken cancellationToken)
    {
        if (command.RoomVolume is < 0 or > 100 || command.MicrophoneGain is < 0 or > 150)
        {
            throw new ArgumentOutOfRangeException(nameof(command));
        }
        var console = RequireConnectedCapability(capabilities => capabilities.AudioControl, "audio control");
        await WriteTransactionAsync(console,
            [$"room-volume {command.RoomVolume}", $"mic-gain {command.MicrophoneGain}", command.NoiseIsolation ? "noise-isolation on" : "noise-isolation off"],
            cancellationToken);
    }

    private Task SetMicrophoneMutedAsync(SetMicrophoneMuted command, CancellationToken cancellationToken)
    {
        var console = RequireConnectedCapability(capabilities => capabilities.MuteControl, "microphone mute control");
        return console.WriteLineAsync(command.IsMuted ? "mic-mute on" : "mic-mute off", cancellationToken);
    }

    private Task SetSystemAudioEnabledAsync(SetSystemAudioEnabled command, CancellationToken cancellationToken)
    {
        var console = RequireConnectedCapability(capabilities => capabilities.SystemAudioControl, "system audio control");
        return console.WriteLineAsync(command.Enabled ? "system-audio on" : "system-audio off", cancellationToken);
    }

    private Task SetRoomAudioMutedAsync(SetRoomAudioMuted command, CancellationToken cancellationToken)
    {
        var console = RequireConnectedCapability(capabilities => capabilities.RoomMixerControl, "room audio control");
        return console.WriteLineAsync(command.IsMuted ? "room-mute on" : "room-mute off", cancellationToken);
    }

    private Task SetRoomMixerAsync(SetRoomMixer command, CancellationToken cancellationToken)
    {
        ValidateRoomMixer(command.MasterPercent, command.BassDb, command.VoiceDb, command.TrebleDb);
        if (command.NoiseReductionDb is < 0 or > 30) throw new ArgumentOutOfRangeException(nameof(command.NoiseReductionDb));
        var console = RequireConnectedCapability(capabilities => capabilities.RoomMixerControl, "room mixer control");
        return console.WriteLineAsync(
            $"room-mixer {command.MasterPercent} {command.BassDb} {command.VoiceDb} {command.TrebleDb}" +
            (command.NoiseReductionDb == 0 ? "" : $" {command.NoiseReductionDb}"),
            cancellationToken);
    }

    private async Task StartRecordingAsync(StartLectureRecording command, CancellationToken cancellationToken)
    {
        var encoded = EncodeOpaqueId(command.SourceId, nameof(command.SourceId));
        IProtocolConsole console;
        lock (_stateLock)
        {
            console = RequireConnectedCapabilityLocked(capabilities => capabilities.Recording, "recording");
            if (_pendingFinalization is not null || _recordStartRequested || _confirmedRecording.State is not (RecordingState.Idle or RecordingState.Completed or RecordingState.Failed))
            {
                throw new InvalidOperationException("The recorder is not quiescent.");
            }
        }
        lock (_stateLock)
        {
            _recordStartRequested = true;
            _requestedRecordingMicrophone = command.IncludeMicrophone;
            _requestedRecordingRoom = command.IncludeRoomAudio;
        }
        try
        {
            await console.WriteLineAsync(
                FormatRecordingStart(command, encoded),
                cancellationToken);
        }
        catch
        {
            lock (_stateLock)
            {
                if (_recordStartRequested)
                {
                    _recordStartRequested = false;
                    _requestedRecordingMicrophone = false;
                    _requestedRecordingRoom = false;
                }
            }
            throw;
        }
    }

    protected virtual string FormatRecordingStart(StartLectureRecording command, string encodedSourceId) =>
        $"record start {encodedSourceId} {(command.IncludeMicrophone ? "mic-on" : "mic-off")} {(command.IncludeRoomAudio ? "room-on" : "room-off")}";

    protected virtual bool AcceptAutomaticRecordingPause => false;
    protected virtual bool AcceptQueuedRecordingBeforeFinalization => false;

    private async Task SetRecordingPausedAsync(bool paused, CancellationToken cancellationToken)
    {
        IProtocolConsole console;
        lock (_stateLock)
        {
            console = RequireConnectedCapabilityLocked(c => c.Recording, "recording pause/resume");
            if (_confirmedRecording.State != RecordingState.Recording || _recordStopRequested)
                throw new InvalidOperationException("Pause/resume requires an active recording.");
            if (_recordPauseRequested is not null) throw new InvalidOperationException("Wait for the recorder to confirm pause/resume.");
            if (_confirmedRecording.Paused == paused) return;
            _recordPauseRequested = paused;
        }
        try { await console.WriteLineAsync(paused ? "record pause" : "record resume", cancellationToken); }
        catch { lock (_stateLock) _recordPauseRequested = null; throw; }
    }

    private async Task StopRecordingAsync(CancellationToken cancellationToken)
    {
        IProtocolConsole? console;
        lock (_stateLock)
        {
            if (_recordStopRequested || _confirmedRecording.State is not (RecordingState.Starting or RecordingState.Recording))
            {
                return;
            }
            console = _activeConsole;
        }
        if (console is null)
        {
            return;
        }
        lock (_stateLock)
        {
            _recordStopRequested = true;
        }
        try
        {
            await console.WriteLineAsync("record stop", cancellationToken);
        }
        catch
        {
            lock (_stateLock)
            {
                if (_confirmedRecording.State is RecordingState.Starting or RecordingState.Recording)
                {
                    _recordStopRequested = false;
                }
            }
            throw;
        }
    }

    private async Task WriteTransactionAsync(IProtocolConsole console, IReadOnlyList<string> commands, CancellationToken cancellationToken)
    {
        foreach (var command in commands)
        {
            await console.WriteLineAsync(command, cancellationToken);
        }
    }

    private Task WriteToActiveAsync(string command, CancellationToken cancellationToken)
    {
        IProtocolConsole console;
        lock (_stateLock)
        {
            console = _activeConsole ?? throw new InvalidOperationException("No incoming or active call is selected.");
        }
        return console.WriteLineAsync(command, cancellationToken);
    }

    private IProtocolConsole RequireConnectedCapability(Func<MediaCapabilitiesChanged, bool> selector, string name)
    {
        lock (_stateLock)
        {
            return RequireConnectedCapabilityLocked(selector, name);
        }
    }

    private IProtocolConsole RequireVideoConsole(bool enabling)
    {
        lock (_stateLock)
        {
            if (_activeConsole is null || !_capabilities.VideoControl || enabling && !_connected)
                throw new NotSupportedException("Native video control is not ready.");
            return _activeConsole;
        }
    }

    private IProtocolConsole RequireConnectedCapabilityLocked(Func<MediaCapabilitiesChanged, bool> selector, string name)
    {
        if (!_connected || _activeConsole is null || !selector(_capabilities))
        {
            throw new NotSupportedException($"Native {name} is not ready for the current call.");
        }
        return _activeConsole;
    }

    private void Console_LineReceived(object? sender, ProtocolConsoleLine line)
    {
        var console = (IProtocolConsole)sender!;
        lock (_stateLock)
        {
            if (_sharedWorkerLifecycle && ReferenceEquals(console, _retiringConsole) &&
                ReferenceEquals(console, _activeConsole) && line.ProcessGeneration == console.ProcessGeneration &&
                !line.IsJsonShaped && line.TryGetLegacyText(out var retiredText) &&
                ProtocolConsoleEventParser.TryParse(console.Protocol, retiredText, _activeRemoteAddress, out var retiredEvent) &&
                retiredEvent is CallEnded or CallFailed)
                _retiringCallTerminated = true;
            if (_disposed || ReferenceEquals(console, _retiringConsole) ||
                _pendingSipDial is { Expired: true } expired && ReferenceEquals(console, expired.Console)) return;
        }
        if (line.IsJsonShaped)
        {
            lock (_stateLock) if (ReferenceEquals(console, _preparingConsole)) return;
            if (line.IsError)
            {
                return;
            }
            if (!NativeJsonEvent.TryParse(line.Bytes.Span, console.Protocol, out var parsed, out _))
            {
                return;
            }
            HandleNativeEvent(console, line.ProcessGeneration, parsed!);
            return;
        }
        if (!line.TryGetLegacyText(out var text))
        {
            return;
        }
        if (text == "video-startup-preferences-ready-v1")
        {
            lock (_stateLock) _videoStartupReady.Add(new ConsoleGeneration(console, line.ProcessGeneration));
            return;
        }
        lock (_stateLock) if (ReferenceEquals(console, _preparingConsole)) return;
        string? fallback;
        lock (_stateLock)
        {
            fallback = ReferenceEquals(console, _activeConsole) ? _activeRemoteAddress : null;
        }
        if (ProtocolConsoleEventParser.TryParse(console.Protocol, text, fallback, out var engineEvent) && engineEvent is not null)
        {
            HandleLegacyEvent(console, line.ProcessGeneration, engineEvent);
        }
    }

    private void HandleNativeEvent(IProtocolConsole console, long processGeneration, NativeJsonEvent parsed)
    {
        lock (_eventDispatchLock)
        {
            EngineEvent? accepted = null;
            lock (_stateLock)
            {
                var key = new ConsoleGeneration(console, processGeneration);
                if (_lastSequences.TryGetValue(key, out var last) && parsed.Sequence <= last)
                {
                    if (_pendingFinalization is { } pending && pending.Matches(console, processGeneration))
                    {
                        _pendingFinalization = null;
                    }
                    return;
                }
                _lastSequences[key] = parsed.Sequence;
                if (parsed.Event is AacAvailabilityChanged or EchoCancellationChanged && processGeneration == console.ProcessGeneration)
                    accepted = parsed.Event;
                else if (parsed.Event is SipRegistrationStateChanged && parsed.Protocol == CallProtocol.Sip &&
                    ReferenceEquals(console, _consoles[CallProtocol.Sip]) && processGeneration == console.ProcessGeneration)
                    accepted = parsed.Event;
                else if (_pendingFinalization is { } lease && lease.Matches(console, processGeneration) &&
                    parsed.Event is RecordingError or RecordingStateChanged)
                {
                    accepted = AcceptPendingFinalizationLocked(parsed, lease);
                    if (_activeConsole is not null)
                    {
                        accepted = null;
                    }
                }
                else if (_dialPreflight is not null && ReferenceEquals(console, _idlePreflightConsole) &&
                    processGeneration == _idlePreflightGeneration && parsed.Event is AudioProfileApplied or NetworkVideoApplied or MediaError)
                {
                    accepted = AcceptCurrentCallEventLocked(parsed);
                }
                else if (ReferenceEquals(console, _activeConsole) && processGeneration == _activeProcessGeneration && parsed.Protocol == _activeProtocol)
                {
                    if (!_connected && IsPreConnectedSnapshot(parsed.Event))
                    {
                        _preConnectedSnapshots.Add(parsed);
                    }
                    else
                    {
                        accepted = AcceptCurrentCallEventLocked(parsed);
                    }
                }
            }
            if (accepted is not null)
            {
                EventReceived?.Invoke(this, accepted);
            }
        }
    }

    private static bool IsPreConnectedSnapshot(EngineEvent engineEvent) =>
        engineEvent is MediaCapabilitiesChanged or AudioControlsChanged or MicrophoneMuteChanged or RecordingStateChanged or VideoStateChanged or AudioDirectionState or ContentSharingStateChanged or SipSecurityStateChanged or SipCallModeChanged or SipAudioUnavailable;

    private EngineEvent? AcceptCurrentCallEventLocked(NativeJsonEvent parsed)
    {
        if (_recordingStreamInvalid && parsed.Event is RecordingError or RecordingStateChanged)
        {
            return null;
        }
        if (_awaitingFailedState && parsed.Event is not RecordingStateChanged { State: RecordingState.Failed })
        {
            _awaitingFailedState = false;
            _recordingStreamInvalid = true;
            return null;
        }
        switch (parsed.Event)
        {
            case CallHoldChanged hold when _connected:
                _held = hold.Held;
                return hold;
            case ContentSharingStateChanged content when _connected:
                return content;
            case SipConnectionHealthChanged health when _connected: return health;
            case OpalVideoTransportChanged transport when _connected: return transport;
            case SipSecurityStateChanged security when _connected: return security;
            case SipCallModeChanged mode when _connected: return mode;
            case SipAudioUnavailable unavailable when _connected: return unavailable;
            case SipOutboundVideoProofChanged proof when _connected: return proof;
            case AudioDirectionState direction when _connected: return direction;
            case AudioProfileApplied profile:
                if (_dialPreflight?.Settings is { } requested &&
                    profile.Codec == (requested.AudioCodec switch { AudioSelection.AacLd => "aac-ld", AudioSelection.AacLc => "aac-lc", AudioSelection.G722 => "g722", _ => "auto" }) &&
                    profile.Channels == requested.AudioChannels && profile.BitrateKbps == requested.AudioKbps &&
                    profile.JitterMinimumMs == requested.JitterMinimumMs && profile.JitterMaximumMs == requested.JitterMaximumMs &&
                    profile.JitterInitialMs == (parsed.Protocol == CallProtocol.H323 ? requested.JitterMinimumMs : requested.JitterInitialMs) &&
                    profile.SendBufferMs == ((requested.SendBufferMs + 19) / 20 * 20) && profile.PacketMs == requested.PacketMs)
                    _dialPreflight.AudioAcknowledgement?.TrySetResult();
                return profile;
            case NetworkVideoApplied network:
                if (_dialPreflight is { } preflight && network.Fps == preflight.Settings.CameraFps &&
                    network.BitrateKbps == preflight.Settings.EffectiveVideoCeilingKbps &&
                    network.Content == (preflight.Settings.ContentMode == ScreenContentMode.Text ? "text" : "motion"))
                    preflight.VideoAcknowledgement?.TrySetResult();
                return network;
            case TransportStatistics statistics when _connected:
                return statistics;
            case MediaError error when _dialPreflight is not null:
                _dialPreflight.Fail(new InvalidOperationException(error.Detail));
                return error;
            case MediaCapabilitiesChanged ready when _connected:
                if (_pendingFinalization is not null && ready.Recording)
                {
                    return null;
                }
                _capabilities = ready;
                if (!ready.Recording)
                {
                    _recordingInitialSnapshotSeen = false;
                    _recordStartRequested = false;
                }
                return ready;
            case LocalSourceChanged source when _connected && _capabilities.SourceControl:
                _confirmedSource = source.Source;
                if (source.Source == VideoSource.Desktop)
                {
                    _confirmedDesktopId = _requestedDesktopId;
                    _requestedDesktopId = null;
                }
                else
                {
                    _confirmedDesktopId = null;
                    _requestedDesktopId = null;
                }
                return source;
            case MuteChanged mute when _connected && _capabilities.MuteControl:
                _confirmedMuted = mute.IsMuted;
                return mute;
            case MicrophoneMuteChanged mute when _connected && _capabilities.MuteControl:
                _confirmedMuted = mute.IsMuted;
                return mute;
            case SystemAudioChanged systemAudio when _connected && _capabilities.SystemAudioControl:
                _confirmedSystemAudioEnabled = systemAudio.Enabled;
                return systemAudio;
            case RoomAudioMuteChanged roomAudio when _connected && _capabilities.RoomMixerControl:
                _confirmedRoomAudioMuted = roomAudio.IsMuted;
                return roomAudio;
            case RoomMixerChanged mixer when _connected && _capabilities.RoomMixerControl:
                _confirmedRoomMixer = mixer;
                return mixer;
            case RoomAudioMeterChanged meter when _connected && _capabilities.RoomMixerControl:
                return meter;
            case MediaStatsChanged stats when _connected && _capabilities.Stats:
                return stats;
            case AudioCodecStateChanged audioCodec when _connected:
                return audioCodec;
            case RemoteSurfaceChanged surface when _connected && _capabilities.RemoteSurface && surface.Epoch > _lastRemoteNativeEpoch:
                _lastRemoteNativeEpoch = surface.Epoch;
                return surface with { Epoch = RegisterSurfaceLocked(surface.Epoch, false) };
            case LocalSurfaceChanged surface when _connected && _capabilities.VideoControl && surface.Epoch > _lastLocalNativeEpoch:
                _lastLocalNativeEpoch = surface.Epoch;
                return surface with { Epoch = RegisterSurfaceLocked(surface.Epoch, true) };
            case VideoStateChanged video when _connected && _capabilities.VideoControl && video.Revision > _videoRevision:
                _videoRevision = video.Revision;
                return video;
            case AudioControlsChanged audio when _connected && _capabilities.AudioControl:
                _confirmedAudio = audio;
                return audio;
            case MediaError error when _connected:
                return error;
            case RecordingError error when _connected && _capabilities.Recording:
                return AcceptRecordingErrorLocked(error);
            case RecordingStateChanged state when _connected && _capabilities.Recording:
                return AcceptRecordingStateLocked(state);
            default:
                return null;
        }
    }

    private EngineEvent? AcceptRecordingErrorLocked(RecordingError error)
    {
        if (error.Code is "recording-pause-rejected" or "recording-resume-rejected")
        {
            if (_recordPauseRequested is null || _confirmedRecording.State != RecordingState.Recording) return null;
            _recordPauseRequested = null;
            return error;
        }
        if (_recordStartRequested && _confirmedRecording.State is RecordingState.Idle or RecordingState.Completed or RecordingState.Failed)
        {
            _recordStartRequested = false;
            _requestedRecordingMicrophone = false;
            _requestedRecordingRoom = false;
            return error;
        }
        if (_confirmedRecording.State is RecordingState.Starting or RecordingState.Recording or RecordingState.Finalizing)
        {
            if (error.Code is not ("microphone-unavailable" or "room-audio-unavailable"))
            {
                _awaitingFailedState = true;
            }
            return error;
        }
        return null;
    }

    private EngineEvent? AcceptRecordingStateLocked(RecordingStateChanged next)
    {
        if (!_recordingInitialSnapshotSeen)
        {
            if (next.State is not (RecordingState.Idle or RecordingState.Completed or RecordingState.Failed))
            {
                return null;
            }
            _recordingInitialSnapshotSeen = true;
            _confirmedRecording = next;
            if (next.FileName.Length != 0)
            {
                _historicalRecordingFileName = next.FileName;
            }
            return next;
        }

        if (next.State == RecordingState.Recording && _confirmedRecording.State == RecordingState.Recording)
        {
            if (next.Paused != _confirmedRecording.Paused && _recordPauseRequested != next.Paused &&
                !(AcceptAutomaticRecordingPause && next.Paused)) return null;
            if (next.Paused == _confirmedRecording.Paused && next.ElapsedMs == _confirmedRecording.ElapsedMs) return null;
            if (_confirmedRecording.Paused && next.Paused && next.ElapsedMs != _confirmedRecording.ElapsedMs) return null;
        }
        var valid = (_confirmedRecording.State, next.State) switch
        {
            (RecordingState.Idle or RecordingState.Completed or RecordingState.Failed, RecordingState.Starting) => _recordStartRequested,
            (RecordingState.Starting, RecordingState.Recording or RecordingState.Finalizing or RecordingState.Failed) => true,
            (RecordingState.Recording, RecordingState.Recording or RecordingState.Finalizing or RecordingState.Failed) => true,
            (RecordingState.Finalizing, RecordingState.Completed or RecordingState.Failed) => true,
            _ => false
        };
        if (next.State == RecordingState.Failed &&
            _confirmedRecording.State is RecordingState.Starting or RecordingState.Recording or RecordingState.Finalizing &&
            !_awaitingFailedState)
        {
            _recordingStreamInvalid = true;
            return null;
        }
        var continuingOperation = _confirmedRecording.State is RecordingState.Starting or RecordingState.Recording or RecordingState.Finalizing;
        if (!valid || continuingOperation && next.ElapsedMs < _confirmedRecording.ElapsedMs ||
            (_awaitingFailedState && next.State != RecordingState.Failed))
        {
            return null;
        }
        if (_confirmedRecording.State is RecordingState.Starting or RecordingState.Recording or RecordingState.Finalizing &&
            (next.MicIncluded != _confirmedRecording.MicIncluded || next.RoomIncluded != _confirmedRecording.RoomIncluded))
        {
            return null;
        }
        if (next.State == RecordingState.Starting)
        {
            if (next.MicIncluded != _requestedRecordingMicrophone || next.RoomIncluded != _requestedRecordingRoom)
            {
                return null;
            }
            _recordStartRequested = false;
            _requestedRecordingMicrophone = false;
            _requestedRecordingRoom = false;
            _recordStopRequested = false;
            _recordingOperationGeneration = checked(_recordingOperationGeneration + 1);
        }
        if (next.State is RecordingState.Finalizing or RecordingState.Completed or RecordingState.Failed)
        {
            _recordStopRequested = false;
        }
        _awaitingFailedState = false;
        _confirmedRecording = next;
        if (next.State != RecordingState.Recording || _recordPauseRequested == next.Paused) _recordPauseRequested = null;
        if (next.FileName.Length != 0)
        {
            _historicalRecordingFileName = next.FileName;
        }
        return next;
    }

    private EngineEvent? AcceptPendingFinalizationLocked(NativeJsonEvent parsed, PendingRecordingFinalization lease)
    {
        if (parsed.Sequence <= lease.LastSequence)
        {
            return null;
        }
        switch (parsed.Event)
        {
            case RecordingStateChanged state when AcceptQueuedRecordingBeforeFinalization &&
                state.State == RecordingState.Recording && !lease.FinalizingAccepted && !lease.AwaitingFailedState &&
                state.ElapsedMs >= lease.ElapsedMs:
                // The shared worker can drain a queued heartbeat/auto-pause after call-cleared.
                // Consume it without restoring live recording state or losing the MP4 finalization lease.
                _pendingFinalization = lease with { LastSequence = parsed.Sequence, ElapsedMs = state.ElapsedMs };
                return null;
            case RecordingError error:
                if (lease.AwaitingFailedState || lease.ErrorAccepted)
                {
                    _pendingFinalization = null;
                    return null;
                }
                var warning = error.Code is "microphone-unavailable" or "room-audio-unavailable";
                _pendingFinalization = lease with
                {
                    LastSequence = parsed.Sequence,
                    ErrorAccepted = true,
                    AwaitingFailedState = !warning
                };
                return error;
            case RecordingStateChanged state when state.State == RecordingState.Finalizing && !lease.FinalizingAccepted && !lease.AwaitingFailedState:
                if (state.ElapsedMs < lease.ElapsedMs)
                {
                    _pendingFinalization = null;
                    return null;
                }
                _pendingFinalization = lease with { LastSequence = parsed.Sequence, FinalizingAccepted = true, ElapsedMs = state.ElapsedMs };
                return state;
            case RecordingStateChanged state when state.State is RecordingState.Completed or RecordingState.Failed:
                if (state.State == RecordingState.Failed && !lease.AwaitingFailedState ||
                    lease.AwaitingFailedState && state.State != RecordingState.Failed || state.ElapsedMs < lease.ElapsedMs)
                {
                    _pendingFinalization = null;
                    return null;
                }
                if (state.FileName.Length != 0)
                {
                    _historicalRecordingFileName = state.FileName;
                }
                _pendingFinalization = null;
                return state;
            default:
                _pendingFinalization = null;
                return null;
        }
    }

    private void HandleLegacyEvent(IProtocolConsole console, long processGeneration, EngineEvent engineEvent)
    {
        lock (_eventDispatchLock)
        {
            var accepted = new List<EngineEvent>();
            var rejectBusy = false;
            var validateActiveLease = false;
            long dispatchCallGeneration = 0;
            lock (_stateLock)
            {
                if (engineEvent is IncomingCall incoming)
                {
                    if (processGeneration != console.ProcessGeneration) return;
                    if (_activeConsole is not null || _deviceTestDeadline?.BlockStart(incoming.RemoteAddress, _timeProvider.GetUtcNow()) == true)
                    {
                        rejectBusy = true;
                    }
                    else
                    {
                        _activeConsole = console;
                        _activeProtocol = incoming.Protocol;
                        _activeProcessGeneration = processGeneration;
                        _activeRemoteAddress = incoming.RemoteAddress;
                        _callGeneration = checked(_callGeneration + 1);
                        ClearLiveMediaLocked();
                        accepted.Add(incoming);
                        validateActiveLease = true;
                        dispatchCallGeneration = _callGeneration;
                    }
                }
                else if (ReferenceEquals(console, _activeConsole) && processGeneration == _activeProcessGeneration)
                {
                    if (engineEvent is CallConnected && _connected) return;
                    accepted.Add(engineEvent);
                    if (engineEvent is CallConnected)
                    {
                        StopSipDialDeadlineLocked();
                        validateActiveLease = true;
                        dispatchCallGeneration = _callGeneration;
                        var snapshots = _preConnectedSnapshots.ToArray();
                        _connected = true;
                        ClearLiveMediaLocked(preserveConnection: true);
                        _capabilities = _sharedWorkerLifecycle ? EmptyCapabilities : ConnectedLocalCapabilities;
                        foreach (var snapshot in snapshots)
                        {
                            if (AcceptCurrentCallEventLocked(snapshot) is { } replayed)
                            {
                                accepted.Add(replayed);
                            }
                        }
                    }
                    else if (engineEvent is CallEnded or CallFailed)
                    {
                        _lastTerminalCallGeneration = _callGeneration;
                        CreateFinalizationLeaseLocked(console, processGeneration);
                        ResetActiveCallLocked();
                    }
                }
            }
            if (rejectBusy)
            {
                _ = RejectBusyAsync(console);
            }
            else
            {
                foreach (var acceptedEvent in accepted)
                {
                    if (validateActiveLease)
                    {
                        lock (_stateLock)
                        {
                            if (!ReferenceEquals(console, _activeConsole) ||
                                processGeneration != _activeProcessGeneration ||
                                dispatchCallGeneration != _callGeneration)
                            {
                                break;
                            }
                        }
                    }
                    EventReceived?.Invoke(this, acceptedEvent);
                }
            }
        }
    }

    private async Task RejectBusyAsync(IProtocolConsole console)
    {
        try
        {
            await WaitForCommandGateAsync(_operationGate, RetirementToken);
            try { await console.WriteLineAsync("reject", CancellationToken.None); }
            finally { _operationGate.Release(); }
        }
        catch (Exception) when (_disposed)
        {
        }
    }

    private void Console_GenerationEnded(object? sender, ProtocolConsoleGenerationEnded ended)
    {
        if (_disposed) return;
        lock (_eventDispatchLock)
        {
            var failedPendingDial = false;
            var failedActiveCall = false;
            lock (_stateLock)
            {
                var console = (IProtocolConsole)sender!;
                if (_sharedWorkerLifecycle && ReferenceEquals(console, _retiringConsole) &&
                    ReferenceEquals(console, _activeConsole) && ended.ProcessGeneration == console.ProcessGeneration)
                    _retiringCallTerminated = true;
                _videoStartupReady.Remove(new ConsoleGeneration(console, ended.ProcessGeneration));
                if (_pendingFinalization is { } lease && lease.Matches(console, ended.ProcessGeneration))
                {
                    _pendingFinalization = null;
                }
                foreach (var epoch in _surfaceAcknowledgements.Where(pair =>
                             ReferenceEquals(pair.Value.Console, console) && pair.Value.Generation == ended.ProcessGeneration).Select(pair => pair.Key).ToArray())
                {
                    _surfaceAcknowledgements.Remove(epoch);
                }
                foreach (var epoch in _localSurfaceAcknowledgements.Where(pair =>
                             ReferenceEquals(pair.Value.Console, console) && pair.Value.Generation == ended.ProcessGeneration).Select(pair => pair.Key).ToArray())
                    _localSurfaceAcknowledgements.Remove(epoch);
                if (ReferenceEquals(_activeConsole, console) && _activeProcessGeneration == ended.ProcessGeneration && !ReferenceEquals(console, _preparingConsole))
                {
                    failedPendingDial = _pendingSipDial is not null;
                    failedActiveCall = !ReferenceEquals(console, _retiringConsole) && _lastTerminalCallGeneration != _callGeneration;
                    if (failedActiveCall) _lastTerminalCallGeneration = _callGeneration;
                    _dialPreflight?.Fail(new IOException("The selected engine exited before confirming audio/video settings."));
                    ResetActiveCallLocked();
                }
            }
            if (failedActiveCall) EventReceived?.Invoke(this, new CallFailed(failedPendingDial ? "sip-engine-exited" : "native-engine-exited"));
        }
    }

    private void CreateFinalizationLeaseLocked(IProtocolConsole console, long processGeneration)
    {
        if (_confirmedRecording.State is not (RecordingState.Starting or RecordingState.Recording or RecordingState.Finalizing))
        {
            return;
        }
        var key = new ConsoleGeneration(console, processGeneration);
        _lastSequences.TryGetValue(key, out var lastSequence);
        _pendingFinalization = new PendingRecordingFinalization(
            console, processGeneration, console.Protocol, lastSequence, _recordingOperationGeneration,
            _confirmedRecording.State == RecordingState.Finalizing, false, false, _confirmedRecording.ElapsedMs);
    }

    private void ClearLiveMediaAtCallStart()
    {
        _held = false;
        lock (_stateLock)
        {
            ClearLiveMediaLocked();
        }
    }

    private void ClearLiveMediaLocked(bool preserveConnection = false)
    {
        if (!preserveConnection) _held = false;
        _videoRevision = 0;
        _lastRemoteNativeEpoch = _lastLocalNativeEpoch = 0;
        _connected = preserveConnection && _connected;
        _capabilities = EmptyCapabilities;
        _confirmedSource = VideoSource.Camera;
        _confirmedMuted = false;
        _confirmedSystemAudioEnabled = false;
        _confirmedRoomAudioMuted = false;
        _confirmedRoomMixer = DefaultRoomMixer;
        _confirmedAudio = new AudioControlsChanged(100, 100, true);
        _confirmedRecording = IdleRecording;
        _recordingInitialSnapshotSeen = false;
        _recordStartRequested = false;
        _recordStopRequested = false;
        _recordPauseRequested = null;
        _awaitingFailedState = false;
        _recordingStreamInvalid = false;
        _requestedRecordingMicrophone = false;
        _requestedRecordingRoom = false;
        _requestedDesktopId = null;
        _confirmedDesktopId = null;
        _preConnectedSnapshots.Clear();
    }

    private ulong RegisterSurfaceLocked(ulong nativeEpoch, bool local)
    {
        var epoch = checked((ulong)Interlocked.Increment(ref _nextPresentationEpoch));
        var owners = local ? _localSurfaceAcknowledgements : _surfaceAcknowledgements;
        owners[epoch] = new SurfaceOwner(_activeConsole!, _activeProcessGeneration, nativeEpoch, _callGeneration);
        return epoch;
    }

    private void ResetActiveCallLocked()
    {
        _dialPreflight?.Fail(new IOException("The call ended before settings were confirmed."));
        _dialPreflight = null;
        StopSipDialDeadlineLocked();
        _preparingConsole = null;
        _activeConsole = null;
        _activeProtocol = null;
        _activeProcessGeneration = 0;
        _activeRemoteAddress = null;
        ClearLiveMediaLocked();
    }

    private static void ValidateDestination(string destination)
    {
        if (string.IsNullOrEmpty(destination) || destination.Contains('\r') || destination.Contains('\n'))
        {
            throw new ArgumentException("Destination must be a nonempty single line.", nameof(destination));
        }
    }

    private static void ValidateRoomMixer(int masterPercent, int bassDb, int voiceDb, int trebleDb)
    {
        if (masterPercent is < 0 or > 300 || bassDb is < -12 or > 12 ||
            voiceDb is < -12 or > 12 || trebleDb is < -12 or > 12)
        {
            throw new ArgumentOutOfRangeException(nameof(masterPercent), "Room mixer values are outside the native contract.");
        }
    }

    private static void ValidateEnums(MediaPreferenceSnapshot value)
    {
        if (!Enum.IsDefined(value.Codec) || !Enum.IsDefined(value.Quality) || !Enum.IsDefined(value.Endpoint))
        {
            throw new ArgumentOutOfRangeException(nameof(value));
        }
    }

    private static void ValidateCodec(CallProtocol protocol, CodecPreference codec, EndpointProfile endpoint)
    {
        ValidateEnums(new MediaPreferenceSnapshot(codec, QualityMode.Auto, endpoint));
        if (codec == CodecPreference.H265 && (protocol == CallProtocol.H323 || IsHardwareH264Only(endpoint)))
        {
            throw new NotSupportedException("H.265 is unavailable for this protocol or endpoint profile.");
        }
    }

    private static bool IsHardwareH264Only(EndpointProfile endpoint) => endpoint is
        EndpointProfile.Ex90 or EndpointProfile.Sx20 or EndpointProfile.SonyXg77 or EndpointProfile.SonyXg80;

    private static string EncodeOpaqueId(string? value, string parameterName)
    {
        if (string.IsNullOrEmpty(value))
        {
            throw new ArgumentException("A nonempty opaque source identifier is required.", parameterName);
        }
        byte[] bytes;
        try { bytes = StrictUtf8.GetBytes(value); }
        catch (EncoderFallbackException error) { throw new ArgumentException("Source identifier is not valid UTF-8 text.", parameterName, error); }
        if (bytes.Length is < 1 or > 1024)
        {
            throw new ArgumentOutOfRangeException(parameterName);
        }
        return Convert.ToBase64String(bytes).TrimEnd('=').Replace('+', '-').Replace('/', '_');
    }

    private static string CodecCommand(CodecPreference value) => "codec " + value switch
    {
        CodecPreference.Auto => "auto", CodecPreference.H264 => "h264", CodecPreference.H265 => "h265",
        _ => throw new ArgumentOutOfRangeException(nameof(value))
    };

    private static string QualityCommand(QualityMode value) => "quality " + value switch
    {
        QualityMode.Auto => "auto", QualityMode.Presentation => "presentation", QualityMode.Motion => "motion", QualityMode.Maximum => "maximum",
        _ => throw new ArgumentOutOfRangeException(nameof(value))
    };

    protected string EndpointCommand(EndpointProfile value, CallProtocol protocol) => "endpoint " + value switch
    {
        EndpointProfile.Auto => "auto", EndpointProfile.Ex90 => "ex90", EndpointProfile.Sx20 => "sx20", EndpointProfile.SonyXg77 => "pcs-xg77",
        EndpointProfile.SonyXg80 => "pcs-xg80",
        EndpointProfile.Generic => "generic", _ => throw new ArgumentOutOfRangeException(nameof(value))
    };

    private static string NetworkModeCommand(NetworkConnectionMode value) => "network-mode " + value switch
    {
        NetworkConnectionMode.Building => "direct",
        NetworkConnectionMode.ExternalVpn => "external",
        _ => throw new ArgumentOutOfRangeException(nameof(value))
    };

    private static readonly MediaCapabilitiesChanged EmptyCapabilities = new(false, false, false, false, false, false);
    private static readonly MediaCapabilitiesChanged ConnectedLocalCapabilities = new(true, true, false, false, false, false);
    private static readonly RoomMixerChanged DefaultRoomMixer = new(100, 0, 0, 0);
    private static readonly RecordingStateChanged IdleRecording = new(RecordingState.Idle, false, false, 0, string.Empty);

    private sealed record MediaPreferenceSnapshot(CodecPreference Codec, QualityMode Quality, EndpointProfile Endpoint);
    private sealed record ConsoleGeneration(IProtocolConsole Console, long Generation)
    {
        public bool Equals(ConsoleGeneration? other) => other is not null && ReferenceEquals(Console, other.Console) && Generation == other.Generation;
        public override int GetHashCode() => HashCode.Combine(RuntimeHelpers.GetHashCode(Console), Generation);
    }
    private sealed record PendingRecordingFinalization(
        IProtocolConsole Console, long ProcessGeneration, CallProtocol Protocol, long LastSequence,
        long RecordingOperationGeneration, bool FinalizingAccepted, bool ErrorAccepted, bool AwaitingFailedState, long ElapsedMs)
    {
        public bool Matches(IProtocolConsole console, long processGeneration) =>
            ReferenceEquals(Console, console) && ProcessGeneration == processGeneration && Protocol == console.Protocol;
    }
}

public interface IRecordingFileBoundary
{
    string GetVideosFolder();
    bool FileExists(string path);
    void OpenFile(string path);
    void OpenFolder(string path);
}

public sealed record RecordedLectureFile(string FileName, DateTime Modified, long Bytes);

public sealed class RecordingFileActions
{
    private readonly IRecordingFileBoundary _boundary;
    private readonly Func<string?>? _selectedDirectory;

    public RecordingFileActions() : this(new WindowsRecordingFileBoundary(), () => TeleStudio.Settings.RecordingLocationStore.ForCurrentUser().Load() ?? (TeleStudio.Settings.AppDataPaths.OverrideRoot is { } isolated ? Path.Combine(isolated, "Recordings") : null))
    {
    }

    public RecordingFileActions(IRecordingFileBoundary boundary, Func<string?>? selectedDirectory = null)
    {
        _boundary = boundary ?? throw new ArgumentNullException(nameof(boundary));
        _selectedDirectory = selectedDirectory;
    }

    public bool TryOpenFile(string fileName, out string? errorCode)
    {
        if (!TryResolveFile(fileName, out var path))
        {
            errorCode = "recording-file-invalid";
            return false;
        }
        if (!_boundary.FileExists(path))
        {
            errorCode = "recording-file-missing";
            return false;
        }
        _boundary.OpenFile(path);
        errorCode = null;
        return true;
    }

    public bool TryOpenFolder(out string? errorCode)
    {
        if (!TryGetFixedFolder(out var folder))
        {
            errorCode = "recording-folder-unavailable";
            return false;
        }
        _boundary.OpenFolder(folder);
        errorCode = null;
        return true;
    }

    public bool TryGetDirectory(out string directory) => TryGetFixedFolder(out directory);
    public IReadOnlyList<RecordedLectureFile> ListCompletedFiles()
    {
        if (!TryGetFixedFolder(out var directory) || !Directory.Exists(directory)) return [];
        if ((File.GetAttributes(directory) & FileAttributes.ReparsePoint) != 0) return [];
        return new DirectoryInfo(directory).EnumerateFiles("*.mp4", SearchOption.TopDirectoryOnly)
            .Where(file => (file.Attributes & FileAttributes.ReparsePoint) == 0 && !file.Name.EndsWith(".part.mp4", StringComparison.OrdinalIgnoreCase) && NativeJsonEvent.IsSafeRecordingFileName(file.Name))
            .OrderByDescending(file => file.LastWriteTimeUtc).Select(file => new RecordedLectureFile(file.Name, file.LastWriteTime, file.Length)).ToArray();
    }

    internal bool TryResolveFile(string fileName, out string path)
    {
        path = string.Empty;
        if (!NativeJsonEvent.IsSafeRecordingFileName(fileName) || !TryGetFixedFolder(out var folder))
        {
            return false;
        }
        try
        {
            var candidate = Path.GetFullPath(Path.Combine(folder, fileName));
            if (!string.Equals(Path.GetDirectoryName(candidate), folder, StringComparison.OrdinalIgnoreCase))
            {
                return false;
            }
            path = candidate;
            return true;
        }
        catch (Exception error) when (error is ArgumentException or NotSupportedException or PathTooLongException)
        {
            return false;
        }
    }

    private bool TryGetFixedFolder(out string folder)
    {
        folder = string.Empty;
        try
        {
            if (_selectedDirectory?.Invoke() is { } selected)
            {
                if (!Path.IsPathFullyQualified(selected)) return false;
                folder=Path.TrimEndingDirectorySeparator(Path.GetFullPath(selected));
                return true;
            }
            var videos = _boundary.GetVideosFolder();
            if (string.IsNullOrEmpty(videos) || !Path.IsPathFullyQualified(videos))
            {
                return false;
            }
            folder = Path.TrimEndingDirectorySeparator(Path.GetFullPath(Path.Combine(videos, "EssaStudio")));
            return string.Equals(Path.GetDirectoryName(folder), Path.TrimEndingDirectorySeparator(Path.GetFullPath(videos)), StringComparison.OrdinalIgnoreCase);
        }
        catch (Exception error) when (error is ArgumentException or NotSupportedException or IOException or UnauthorizedAccessException or System.Text.Json.JsonException)
        {
            return false;
        }
    }
}

internal sealed class WindowsRecordingFileBoundary : IRecordingFileBoundary
{
    public string GetVideosFolder() => WindowsKnownFolders.GetVideosFolder();
    public bool FileExists(string path) => File.Exists(path);
    public void OpenFile(string path) => System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(path) { UseShellExecute = true });
    public void OpenFolder(string path) => System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(path) { UseShellExecute = true });
}

internal static partial class WindowsKnownFolders
{
    private static readonly Guid VideosFolderId = new("18989B1D-99B5-455B-841C-AB7C74E4DDFC");

    internal static string GetVideosFolder()
    {
        var id = VideosFolderId;
        var result = SHGetKnownFolderPath(ref id, 0, IntPtr.Zero, out var pointer);
        if (result != 0 || pointer == IntPtr.Zero)
        {
            throw new InvalidOperationException("Known Videos folder is unavailable.");
        }
        try
        {
            return Marshal.PtrToStringUni(pointer) ?? throw new InvalidOperationException("Known Videos folder is unavailable.");
        }
        finally
        {
            Marshal.FreeCoTaskMem(pointer);
        }
    }

    [LibraryImport("shell32.dll")]
    private static partial int SHGetKnownFolderPath(ref Guid rfid, uint flags, IntPtr token, out IntPtr path);
}

