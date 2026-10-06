using System.ComponentModel;
using System.Runtime.CompilerServices;
using TeleStudio.Contracts;

namespace TeleStudio.Presentation;

public sealed class MainViewModel : INotifyPropertyChanged
{
    public const string ProductName = "EssaStudio";
    public const string RightsStatement = "جميع حقوق النسخ محفوظة لمصمم التطبيق الأستاذ. عيسى يحي مهارش";
    private readonly IEngineCommandSink _engine;
    private readonly Action<string>? _acceptedLecturerName;
    private string _lecturerName = ProductName;
    private string _destination = string.Empty;
    private CallProtocol _selectedProtocol = CallProtocol.Sip;
    private EndpointProfile _selectedEndpoint = EndpointProfile.Auto;
    private CodecPreference _selectedCodec = CodecPreference.Auto;
    private QualityMode _selectedQuality = QualityMode.Auto;
    private NetworkConnectionMode _selectedNetworkMode = NetworkConnectionMode.Building;
    private CallSession _session = CallSession.Idle;
    private AppLanguage _language = AppLanguage.English;
    private bool _isMuted;
    private bool _sourcePending;
    private VideoSource? _pendingSource;
    private bool _mutePending;
    private bool _sipH265CapabilityConfirmed;
    private bool _preferencesDirty;
    private EndpointProfile? _activeEndpointProfile;
    private string _codecCompatibilityNote = string.Empty;
    private MediaStatsChanged? _mediaStats;
    private RemoteSurfaceChanged? _remoteSurface;
    private AudioCodecStateChanged? _audioCodecState;

    public MainViewModel(IEngineCommandSink engine, Action<string>? acceptedLecturerName = null)
    {
        _engine = engine ?? throw new ArgumentNullException(nameof(engine));
        _acceptedLecturerName = acceptedLecturerName;
        SetCompatibilityNote();
    }

    public event PropertyChangedEventHandler? PropertyChanged;

    public string Destination
    {
        get => _destination;
        set { if (SetField(ref _destination, value ?? string.Empty)) OnPropertyChanged(nameof(CanDial)); }
    }

    public CallProtocol SelectedProtocol
    {
        get => _selectedProtocol;
        set
        {
            if (!SetField(ref _selectedProtocol, value)) return;
            OnPropertyChanged(nameof(CanDial));
            ResetSipH265Capability();
            _preferencesDirty = true;
            EnforceCodecCompatibility();
            NotifyCodecAvailability();
        }
    }

    public EndpointProfile SelectedEndpoint
    {
        get => _selectedEndpoint;
        set
        {
            if (!SetField(ref _selectedEndpoint, value)) return;
            ResetSipH265Capability();
            _preferencesDirty = true;
            EnforceCodecCompatibility();
            NotifyCodecAvailability();
        }
    }

    public CodecPreference SelectedCodec
    {
        get => _selectedCodec;
        set
        {
            if (value == CodecPreference.H265 && !CanSelectH265) { SetCompatibilityNote(); return; }
            if (SetField(ref _selectedCodec, value)) _preferencesDirty = true;
        }
    }

    public QualityMode SelectedQuality
    {
        get => _selectedQuality;
        set { if (SetField(ref _selectedQuality, value)) _preferencesDirty = true; }
    }

    public NetworkConnectionMode SelectedNetworkMode
    {
        get => _selectedNetworkMode;
        set => SetField(ref _selectedNetworkMode, value);
    }

    public string LecturerName => _lecturerName;
    public string? SelectedDesktopSourceId { get; set; }

    public CallSession Session
    {
        get => _session;
        private set
        {
            if (!SetField(ref _session, value)) return;
            foreach (var name in new[] { nameof(CanDial), nameof(IsHomeVisible), nameof(IsCallVisible), nameof(IsConnected), nameof(IsPending), nameof(IsDesktopSharing), nameof(SourceActionKey), nameof(ResolutionText), nameof(CanAnswer), nameof(CanReject), nameof(CanHangUp) })
                OnPropertyChanged(name);
            NotifyMediaActionAvailability();
        }
    }

    public AppLanguage Language => _language;
    public bool IsRightToLeft => Language == AppLanguage.Arabic;
    public string DestinationFlowDirection => "LeftToRight";
    public string IdentityFlowDirection => "LeftToRight";
    public bool CanDial => DestinationValidator.IsValid(Destination, SelectedProtocol) && Session.State is CallState.Idle or CallState.Failed;
    public bool IsHomeVisible => Session.State is CallState.Idle or CallState.Failed;
    public bool IsCallVisible => !IsHomeVisible;
    public bool IsConnected => Session.State is CallState.ConnectedCamera or CallState.ConnectedDesktop;
    public bool IsPending => IsCallVisible && !IsConnected;
    public bool IsDesktopSharing => Session.State == CallState.ConnectedDesktop;
    public bool CanAnswer => Session.State == CallState.Incoming;
    public bool CanReject => Session.State == CallState.Incoming;
    public bool CanHangUp => Session.State is CallState.Incoming or CallState.Calling or CallState.Negotiating or CallState.ConnectedCamera or CallState.ConnectedDesktop;
    public bool CanToggleSource => IsConnected && Session.SourceControlAvailable &&
        (CanCancelPendingDesktopShare || (!_sourcePending && (IsDesktopSharing || !string.IsNullOrEmpty(SelectedDesktopSourceId))));
    public bool CanToggleMute => IsConnected && Session.MuteControlAvailable && !_mutePending;
    public bool IsSourcePending => _sourcePending;
    public bool CanCancelPendingDesktopShare => _sourcePending && _pendingSource == VideoSource.Desktop;
    public bool IsMutePending => _mutePending;
    public bool IsMuted => _isMuted;
    public bool CanSelectH265 => SelectedProtocol == CallProtocol.Sip && SelectedEndpoint is EndpointProfile.Auto or EndpointProfile.Generic && _sipH265CapabilityConfirmed;
    public string CodecCompatibilityNote => _codecCompatibilityNote;
    public string SourceActionKey => IsDesktopSharing || CanCancelPendingDesktopShare ? "ReturnToCamera" : "ShareDesktop";
    public string ResolutionText => Session.VideoWidth == 0 ? "—" : $"{Session.VideoWidth}×{Session.VideoHeight} · {Session.FramesPerSecond}fps";
    public MediaStatsChanged? MediaStats => _mediaStats;
    public RemoteSurfaceChanged? RemoteSurface => _remoteSurface;
    public AudioCodecStateChanged? AudioCodecState => _audioCodecState;

    public bool TryAcceptLecturerName(string value, out string? bilingualError)
    {
        try
        {
            var normalized = LecturerNameCodec.Normalize(value);
            if (SetField(ref _lecturerName, normalized, nameof(LecturerName))) _acceptedLecturerName?.Invoke(normalized);
            bilingualError = null;
            return true;
        }
        catch (ArgumentException)
        {
            bilingualError = "اكتب اسماً إنجليزياً من 2 إلى 64 حرفاً / Use 2–64 English letters, digits, spaces, period, hyphen or apostrophe.";
            return false;
        }
    }

    public void ConfirmSipH265Capability(bool available)
    {
        if (_sipH265CapabilityConfirmed == available) return;
        _sipH265CapabilityConfirmed = available;
        EnforceCodecCompatibility();
        NotifyCodecAvailability();
    }

    public void SelectRoom(string destination, EndpointProfile endpoint) { Destination = destination; SelectedEndpoint = endpoint; }

    public async Task DialAsync(CancellationToken cancellationToken)
    {
        if (!CanDial) throw new InvalidOperationException("A valid destination is required while idle.");
        if (SelectedCodec == CodecPreference.H265 && !CanSelectH265) throw new NotSupportedException("H.265 is not available for the selected protocol or endpoint.");
        var destination = Destination.Trim();
        _activeEndpointProfile = SelectedEndpoint;
        Session = CallSession.Idle.Apply(new CallProgress(destination, SelectedProtocol, CallState.Calling));
        try
        {
            await _engine.SendAsync(new SetNetworkMode(SelectedNetworkMode), cancellationToken);
            if (_preferencesDirty)
            {
                await _engine.SendAsync(new SetMediaPreferences(SelectedCodec, SelectedQuality, SelectedEndpoint), cancellationToken);
                _preferencesDirty = false;
            }
            await _engine.SendAsync(new Dial(destination, SelectedProtocol, LecturerName), cancellationToken);
        }
        catch (Exception exception)
        {
            Session = Session.Apply(new CallFailed("engine-send-failed", exception.Message));
            throw;
        }
    }

    public async Task ToggleSourceAsync(CancellationToken cancellationToken)
    {
        if (!CanToggleSource) throw new InvalidOperationException("Desktop source control is not ready for this call.");
        var target = IsDesktopSharing || CanCancelPendingDesktopShare ? VideoSource.Camera : VideoSource.Desktop;
        _sourcePending = true;
        _pendingSource = target;
        NotifyMediaActionAvailability();
        try { await _engine.SendAsync(new SwitchSource(target, target == VideoSource.Desktop ? SelectedDesktopSourceId : null), cancellationToken); }
        catch { _sourcePending = false; _pendingSource = null; NotifyMediaActionAvailability(); throw; }
    }

    public Task HangUpAsync(CancellationToken cancellationToken) => _engine.SendAsync(new HangUp(), cancellationToken);
    public Task AnswerAsync(CancellationToken cancellationToken) => CanAnswer ? _engine.SendAsync(new Answer(), cancellationToken) : throw new InvalidOperationException("An incoming call is required before answering.");
    public Task RejectAsync(CancellationToken cancellationToken) => CanReject ? _engine.SendAsync(new Reject(), cancellationToken) : throw new InvalidOperationException("An incoming call is required before rejecting.");

    public async Task ToggleMuteAsync(CancellationToken cancellationToken)
    {
        if (!CanToggleMute) throw new InvalidOperationException("Mute control is not ready for this call.");
        _mutePending = true;
        NotifyMediaActionAvailability();
        try { await _engine.SendAsync(new SetMuted(!IsMuted), cancellationToken); }
        catch { _mutePending = false; NotifyMediaActionAvailability(); throw; }
    }

    public void ApplyEvent(EngineEvent engineEvent)
    {
        ArgumentNullException.ThrowIfNull(engineEvent);
        if (engineEvent is IncomingCall && Session.State != CallState.Idle) return;
        if (Session.State is CallState.Ending or CallState.Failed && engineEvent is CallProgress or CallConnected) return;
        switch (engineEvent)
        {
            // MainWindow routes these to the composed-video model/presenter after this call.
            // They are not legacy call-state transitions; rejecting them aborts that dispatch.
            case VideoStateChanged or LocalSurfaceChanged: return;
            case AudioCodecStateChanged audioCodec when IsConnected:
                _audioCodecState = audioCodec;
                OnPropertyChanged(nameof(AudioCodecState));
                return;
            case AudioCodecStateChanged: return;
            case MediaError error when IsIndependentAudioError(error.Code): return;
            case MediaError: ClearSourcePending(); return;
            case RecordingError: return;
            case RecordingStateChanged when !IsConnected: return;
            case RemoteSurfaceChanged surface: _remoteSurface = surface; OnPropertyChanged(nameof(RemoteSurface)); return;
            case MediaStatsChanged stats when IsConnected:
                _mediaStats = stats;
                OnPropertyChanged(nameof(MediaStats));
                Session = Session with { VideoWidth = stats.VideoWidth, VideoHeight = stats.VideoHeight, FramesPerSecond = stats.FramesPerSecond };
                if (Session.Protocol == CallProtocol.Sip && stats.Codec == CodecPreference.H265 &&
                    _activeEndpointProfile is EndpointProfile.Auto or EndpointProfile.Generic)
                    ConfirmSipH265Capability(true);
                return;
            case MediaStatsChanged: return;
            case MuteChanged changed when IsConnected:
                _isMuted = changed.IsMuted; _mutePending = false; OnPropertyChanged(nameof(IsMuted)); Session = Session.Apply(changed); NotifyMediaActionAvailability(); return;
            case MicrophoneMuteChanged changed when IsConnected:
                _isMuted = changed.IsMuted; _mutePending = false; OnPropertyChanged(nameof(IsMuted)); Session = Session.Apply(changed); NotifyMediaActionAvailability(); return;
            case LocalSourceChanged changed when IsConnected:
                _sourcePending = false; _pendingSource = null; Session = Session.Apply(changed); NotifyMediaActionAvailability(); return;
            case CallEnded or CallFailed: ClearLiveMedia(); break;
            case CallConnected connected:
                _activeEndpointProfile ??= SelectedEndpoint;
                Session = Session.Apply(connected);
                Session = Session.Apply(ConnectedLocalCapabilities);
                return;
            case MediaCapabilitiesChanged capabilities when IsConnected:
                Session = Session.Apply(capabilities);
                return;
        }
        Session = Session.Apply(engineEvent);
    }

    public void SetLanguage(AppLanguage language)
    {
        if (_language == language) return;
        _language = language; OnPropertyChanged(nameof(Language)); OnPropertyChanged(nameof(IsRightToLeft));
    }

    private void EnforceCodecCompatibility()
    {
        if (_selectedCodec == CodecPreference.H265 && !CanSelectH265)
        {
            _selectedCodec = CodecPreference.Auto; _preferencesDirty = true; OnPropertyChanged(nameof(SelectedCodec));
        }
        SetCompatibilityNote();
    }

    private void SetCompatibilityNote()
    {
        var value = CanSelectH265 ? string.Empty : SelectedProtocol == CallProtocol.H323
            ? "H.323 يدعم H.264 فقط / H.323 uses H.264 only."
            : SelectedEndpoint is not (EndpointProfile.Auto or EndpointProfile.Generic)
                ? "هذا الجهاز يدعم H.264 فقط / This endpoint uses H.264 only."
                : "H.265 يحتاج تأكيد المحرك / H.265 requires native capability confirmation.";
        SetField(ref _codecCompatibilityNote, value, nameof(CodecCompatibilityNote));
    }

    private void NotifyCodecAvailability() { OnPropertyChanged(nameof(CanSelectH265)); OnPropertyChanged(nameof(CodecCompatibilityNote)); }
    private void ResetSipH265Capability()
    {
        if (!_sipH265CapabilityConfirmed) return;
        _sipH265CapabilityConfirmed = false;
        NotifyCodecAvailability();
    }
    private void NotifyMediaActionAvailability()
    {
        OnPropertyChanged(nameof(CanToggleSource)); OnPropertyChanged(nameof(CanToggleMute)); OnPropertyChanged(nameof(IsSourcePending)); OnPropertyChanged(nameof(CanCancelPendingDesktopShare)); OnPropertyChanged(nameof(SourceActionKey)); OnPropertyChanged(nameof(IsMutePending));
    }

    private static readonly MediaCapabilitiesChanged ConnectedLocalCapabilities =
        new(true, true, false, false, false, false);
    private void ClearSourcePending() { _sourcePending = false; _pendingSource = null; NotifyMediaActionAvailability(); }
    private static bool IsIndependentAudioError(string code) =>
        string.Equals(code, "audio-not-ready", StringComparison.Ordinal) ||
        code.StartsWith("microphone-mute-", StringComparison.Ordinal) ||
        code.StartsWith("mic-mute-", StringComparison.Ordinal) ||
        code.StartsWith("system-audio-", StringComparison.Ordinal) ||
        code.StartsWith("room-mute-", StringComparison.Ordinal) ||
        code.StartsWith("room-mixer-", StringComparison.Ordinal);
    private void ClearPendingActions() { _sourcePending = false; _pendingSource = null; _mutePending = false; NotifyMediaActionAvailability(); }
    private void ClearLiveMedia()
    {
        ClearPendingActions(); _isMuted = false; _mediaStats = null; _remoteSurface = null; _audioCodecState = null; _activeEndpointProfile = null;
        ResetSipH265Capability();
        OnPropertyChanged(nameof(IsMuted)); OnPropertyChanged(nameof(MediaStats)); OnPropertyChanged(nameof(RemoteSurface)); OnPropertyChanged(nameof(AudioCodecState));
    }
    private bool SetField<T>(ref T field, T value, [CallerMemberName] string? propertyName = null)
    {
        if (EqualityComparer<T>.Default.Equals(field, value)) return false;
        field = value; OnPropertyChanged(propertyName); return true;
    }
    private void OnPropertyChanged([CallerMemberName] string? propertyName = null) => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
}
