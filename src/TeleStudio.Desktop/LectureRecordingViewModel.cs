using System.Collections.ObjectModel;
using System.ComponentModel;
using System.IO;
using System.Runtime.CompilerServices;
using TeleStudio.Contracts;
using TeleStudio.Presentation;

namespace TeleStudio.Desktop;

public interface IRecordingClock { long GetMilliseconds(); }
public interface IRecordingTimer : IDisposable { void Start(Action tick); void Stop(); }
public interface IRecordingFileActionsSurface { bool OpenFile(string basename); bool OpenFolder(); }

public sealed class MonotonicRecordingClock : IRecordingClock
{
    public long GetMilliseconds() => Environment.TickCount64;
}

public sealed class RecordingUiTimer : IRecordingTimer
{
    private readonly SynchronizationContext? _context = SynchronizationContext.Current;
    private Timer? _timer;
    public void Start(Action tick)
    {
        Stop();
        _timer = new Timer(_ => { if (_context is null) tick(); else _context.Post(_ => tick(), null); }, null, 250, 250);
    }
    public void Stop() => Interlocked.Exchange(ref _timer, null)?.Dispose();
    public void Dispose() => Stop();
}

public sealed class TrustedRecordingFileActions : IRecordingFileActionsSurface
{
    private readonly RecordingFileActions _actions = new();
    public bool OpenFile(string basename) => _actions.TryOpenFile(basename, out _);
    public bool OpenFolder() => _actions.TryOpenFolder(out _);
}

public sealed class LectureRecordingViewModel : INotifyPropertyChanged, IDisposable
{
    private readonly IEngineCommandSink _engine;
    private readonly ICaptureCatalog _catalog;
    private readonly IRecordingClock _clock;
    private readonly IRecordingTimer _timer;
    private readonly IRecordingFileActionsSurface _files;
    private IReadOnlyList<CaptureCatalogItem> _items = Array.Empty<CaptureCatalogItem>();
    private CaptureCatalogItem? _selectedItem;
    private bool _includeMicrophone = true;
    private bool _includeRoomAudio;
    private bool _isCallConnected;
    private bool _recordingReady;
    private bool _pendingDetachedFinalization;
    private bool _isMuted;
    private bool _sheetOpen;
    private bool _startRequested;
    private bool _stopRequested;
    private bool _optionsLocked;
    private RecordingState _state = RecordingState.Idle;
    private bool _confirmedMicIncluded;
    private bool _confirmedRoomIncluded;
    private long _confirmedElapsedMs;
    private long _elapsedMs;
    private long _lastClockMs;
    private string _fileName = string.Empty;
    private string _errorMessage = string.Empty;
    private bool _recoverablePart;
    private string? _confirmedSharedDisplayId;

    public LectureRecordingViewModel(IEngineCommandSink engine, ICaptureCatalog catalog, IRecordingClock clock,
        IRecordingTimer timer, IRecordingFileActionsSurface files)
    {
        _engine = engine ?? throw new ArgumentNullException(nameof(engine));
        _catalog = catalog ?? throw new ArgumentNullException(nameof(catalog));
        _clock = clock ?? throw new ArgumentNullException(nameof(clock));
        _timer = timer ?? throw new ArgumentNullException(nameof(timer));
        _files = files ?? throw new ArgumentNullException(nameof(files));
    }

    public event PropertyChangedEventHandler? PropertyChanged;
    public IReadOnlyList<CaptureCatalogItem> Items => _items;
    public CaptureCatalogItem? SelectedItem => _selectedItem;
    public bool IncludeMicrophone { get => _includeMicrophone; set { if (!_optionsLocked) SetField(ref _includeMicrophone, value); } }
    public bool IncludeRoomAudio { get => _includeRoomAudio; set { if (!_optionsLocked) SetField(ref _includeRoomAudio, value); } }
    public bool IsCallConnected => _isCallConnected;
    public bool IsSheetOpen => _sheetOpen;
    public bool OptionsLocked => _optionsLocked;
    public RecordingState State => _state;
    public long ElapsedMs => _elapsedMs;
    public string ElapsedText => TimeSpan.FromMilliseconds(_elapsedMs).ToString(@"hh\:mm\:ss");
    public string ErrorMessage => _errorMessage;
    public bool IsRecordingIndicatorVisible => State == RecordingState.Recording;
    public bool CanRecord => _isCallConnected && _recordingReady && !_pendingDetachedFinalization && !_startRequested &&
        State is RecordingState.Idle or RecordingState.Completed or RecordingState.Failed;
    public bool CanOpenSheet => CanRecord || State is RecordingState.Starting or RecordingState.Recording or RecordingState.Finalizing;
    public bool CanStop => !_stopRequested && State is RecordingState.Starting or RecordingState.Recording;
    public bool CanOpenFile => State == RecordingState.Completed && IsSafeBasename(_fileName, ".mp4");
    public bool CanOpenPart => State == RecordingState.Failed && _recoverablePart && IsSafeBasename(_fileName, ".part");
    public bool IsMuteRecordingWarningVisible => _isMuted &&
        (State == RecordingState.Recording ? _confirmedMicIncluded : IncludeMicrophone) &&
        State is RecordingState.Idle or RecordingState.Starting or RecordingState.Recording;
    public string MuteRecordingWarning => "إرسال المكالمة مكتوم، لكن تسجيل الميكروفون مفعّل / Call transmission is muted while microphone recording is enabled.";
    public bool MicIncluded => State == RecordingState.Starting ? IncludeMicrophone : _confirmedMicIncluded;
    public bool RoomIncluded => State == RecordingState.Starting ? IncludeRoomAudio : _confirmedRoomIncluded;

    public void UpdateCallState(bool connected, bool recordingReady, bool pendingDetachedFinalization, string? confirmedSharedDisplayId, bool isMuted)
    {
        _isCallConnected = connected;
        _recordingReady = recordingReady;
        _pendingDetachedFinalization = pendingDetachedFinalization;
        _confirmedSharedDisplayId = confirmedSharedDisplayId;
        _isMuted = isMuted;
        if (!connected)
        {
            _recordingReady = false;
            if (!pendingDetachedFinalization && State is RecordingState.Idle or RecordingState.Completed or RecordingState.Failed)
                _sheetOpen = false;
        }
        NotifyAll();
    }

    public async Task OpenSheetAsync(CancellationToken cancellationToken)
    {
        if (State is RecordingState.Starting or RecordingState.Recording or RecordingState.Finalizing)
        {
            _sheetOpen = true;
            OnPropertyChanged(nameof(IsSheetOpen));
            return;
        }
        if (!CanRecord) throw new InvalidOperationException("Recording is not ready for this call.");
        _sheetOpen = true;
        _includeMicrophone = true;
        _includeRoomAudio = false;
        _optionsLocked = false;
        _errorMessage = string.Empty;
        _selectedItem = null;
        await RefreshCoreAsync(true, cancellationToken);
        NotifyAll();
    }

    public Task RefreshAsync(CancellationToken cancellationToken) => RefreshCoreAsync(false, cancellationToken);

    private async Task RefreshCoreAsync(bool opening, CancellationToken cancellationToken)
    {
        if (_optionsLocked) return;
        var priorId = _selectedItem?.Id;
        var snapshot = await _catalog.RefreshAsync(cancellationToken);
        if (!snapshot.IsAvailable)
        {
            _items = Array.Empty<CaptureCatalogItem>();
            _selectedItem = null;
            _errorMessage = "تعذر تحديث مصادر التسجيل / Capture sources are unavailable.";
            NotifyAll();
            return;
        }
        _items = new ReadOnlyCollection<CaptureCatalogItem>(snapshot.Items.ToArray());
        if (opening)
        {
            _selectedItem = _confirmedSharedDisplayId is null ? null : _items.FirstOrDefault(item => item.Kind == CaptureItemKind.Monitor && item.Id == _confirmedSharedDisplayId);
            _selectedItem ??= _items.FirstOrDefault(item => item.Kind == CaptureItemKind.Monitor);
        }
        else if (priorId is not null)
        {
            _selectedItem = _items.FirstOrDefault(item => item.Id == priorId);
            if (_selectedItem is null) _errorMessage = "المصدر المحدد لم يعد متاحاً؛ اختره مجدداً / The selected target is no longer available; select it again.";
        }
        OnPropertyChanged(nameof(Items));
        OnPropertyChanged(nameof(SelectedItem));
        OnPropertyChanged(nameof(ErrorMessage));
    }

    public bool SelectItem(string id)
    {
        if (_optionsLocked) return false;
        var item = _items.FirstOrDefault(candidate => candidate.Id == id);
        if (item is null) return false;
        _selectedItem = item;
        _errorMessage = string.Empty;
        OnPropertyChanged(nameof(SelectedItem)); OnPropertyChanged(nameof(ErrorMessage));
        return true;
    }

    public async Task StartAsync(CancellationToken cancellationToken)
    {
        if (_startRequested) return;
        if (!CanRecord || _selectedItem is null) throw new InvalidOperationException("Select a current display or window before recording.");
        _startRequested = true;
        _optionsLocked = true;
        NotifyAll();
        try { await _engine.SendAsync(new StartLectureRecording(_selectedItem.Id, IncludeMicrophone, IncludeRoomAudio), cancellationToken); }
        catch { _startRequested = false; _optionsLocked = false; NotifyAll(); throw; }
    }

    public async Task StopAsync(CancellationToken cancellationToken)
    {
        if (!CanStop) return;
        _stopRequested = true;
        NotifyAll();
        try { await _engine.SendAsync(new StopLectureRecording(), cancellationToken); }
        catch { _stopRequested = false; NotifyAll(); throw; }
    }

    public bool ApplyState(RecordingStateChanged update)
    {
        var beginsNewOperation = update.State == RecordingState.Starting && State is RecordingState.Idle or RecordingState.Completed or RecordingState.Failed;
        if (beginsNewOperation && !_startRequested) return false;
        if (update.State == State && update.ElapsedMs <= _confirmedElapsedMs) return false;
        if (!IsAllowedTransition(State, update.State)) return false;
        if (!beginsNewOperation && update.ElapsedMs < _confirmedElapsedMs) return false;
        if (beginsNewOperation)
        {
            _confirmedElapsedMs = 0;
            _elapsedMs = 0;
            _fileName = string.Empty;
            _recoverablePart = false;
        }
        _state = update.State;
        _confirmedElapsedMs = update.ElapsedMs;
        _elapsedMs = Math.Max(_elapsedMs, _confirmedElapsedMs);
        _confirmedMicIncluded = update.MicIncluded;
        _confirmedRoomIncluded = update.RoomIncluded;
        _fileName = update.FileName;
        _startRequested = false;
        if (State is RecordingState.Starting or RecordingState.Recording or RecordingState.Finalizing) _optionsLocked = true;
        if (State == RecordingState.Recording)
        {
            _lastClockMs = _clock.GetMilliseconds();
            _timer.Start(OnTimerTick);
        }
        else _timer.Stop();
        if (State is RecordingState.Completed or RecordingState.Failed or RecordingState.Idle)
        {
            _stopRequested = false;
            _pendingDetachedFinalization = false;
        }
        NotifyAll();
        return true;
    }

    public void ApplyError(RecordingError error)
    {
        var rejectedPendingStart = State == RecordingState.Idle && _startRequested;
        if (rejectedPendingStart)
        {
            _startRequested = false;
            _optionsLocked = false;
        }
        _errorMessage = string.IsNullOrWhiteSpace(error.Detail)
            ? "تعذر إكمال التسجيل / Recording could not be completed."
            : error.Detail;
        _recoverablePart = error.RecoverablePart;
        _fileName = error.RecoverablePart ? error.FileName : string.Empty;
        if (rejectedPendingStart) NotifyAll();
        else { OnPropertyChanged(nameof(ErrorMessage)); OnPropertyChanged(nameof(CanOpenPart)); }
    }

    public bool OpenFile() => CanOpenFile && _files.OpenFile(_fileName);
    public bool OpenPart() => CanOpenPart && _files.OpenFile(_fileName);
    public bool OpenFolder() => (CanOpenFile || CanOpenPart) && _files.OpenFolder();
    public void CloseUnstartedSheet()
    {
        _sheetOpen = false;
        OnPropertyChanged(nameof(IsSheetOpen));
    }
    public void Dispose() => _timer.Dispose();

    private void OnTimerTick()
    {
        if (State != RecordingState.Recording) return;
        var now = _clock.GetMilliseconds();
        var delta = Math.Max(0, now - _lastClockMs);
        _lastClockMs = now;
        _elapsedMs = delta > long.MaxValue - _elapsedMs ? long.MaxValue : _elapsedMs + delta;
        OnPropertyChanged(nameof(ElapsedMs)); OnPropertyChanged(nameof(ElapsedText));
    }

    private static bool IsAllowedTransition(RecordingState current, RecordingState next) => current switch
    {
        RecordingState.Idle or RecordingState.Completed or RecordingState.Failed => next is RecordingState.Starting or RecordingState.Idle,
        RecordingState.Starting => next is RecordingState.Recording or RecordingState.Finalizing or RecordingState.Failed,
        RecordingState.Recording => next is RecordingState.Recording or RecordingState.Finalizing or RecordingState.Failed,
        RecordingState.Finalizing => next is RecordingState.Finalizing or RecordingState.Completed or RecordingState.Failed,
        _ => false
    };

    private static bool IsSafeBasename(string value, string extension) => value.Length is > 0 and <= 255 &&
        Path.GetFileName(value) == value && value.EndsWith(extension, StringComparison.OrdinalIgnoreCase) &&
        !value.Any(character => character < 32 || "\\/:*?\"<>|".Contains(character)) && value is not "." and not "..";
    private void NotifyAll()
    {
        foreach (var property in new[] { nameof(IsCallConnected), nameof(IsSheetOpen), nameof(OptionsLocked), nameof(State), nameof(ElapsedMs), nameof(ElapsedText), nameof(IsRecordingIndicatorVisible), nameof(CanRecord), nameof(CanOpenSheet), nameof(CanStop), nameof(CanOpenFile), nameof(CanOpenPart), nameof(IsMuteRecordingWarningVisible), nameof(MicIncluded), nameof(RoomIncluded), nameof(IncludeMicrophone), nameof(IncludeRoomAudio), nameof(ErrorMessage) }) OnPropertyChanged(property);
    }
    private bool SetField<T>(ref T field, T value, [CallerMemberName] string? name = null)
    {
        if (EqualityComparer<T>.Default.Equals(field, value)) return false;
        field = value; OnPropertyChanged(name); return true;
    }
    private void OnPropertyChanged([CallerMemberName] string? name = null) => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
}
