using System.Globalization;
using System.IO;
using TeleStudio.Contracts;

namespace TeleStudio.Desktop;

/// <summary>Two protocol views own one process, one command stream and one active call.</summary>
public sealed class SharedOpalConsole
{
    private readonly IProtocolConsole _worker;
    private readonly TimeSpan _terminationTimeout;
    private readonly SemaphoreSlim _gate = new(1, 1);
    private readonly CancellationTokenSource _retirement = new();
    private readonly object _stateLock = new();
    private readonly View _sip, _h323;
    private bool _started;
    private int _viewsReleased;
    private readonly object _releaseLock = new();
    private Task? _releaseTask;
    private CallProtocol? _selected, _callProtocol;
    private long _sequence, _generation;
    private TaskCompletionSource? _terminal;

    public SharedOpalConsole(IProtocolConsole worker) : this(worker, OpalCallLifecycle.TerminationTimeout) { }

    internal SharedOpalConsole(IProtocolConsole worker, TimeSpan terminationTimeout)
    {
        _worker = worker ?? throw new ArgumentNullException(nameof(worker));
        if (terminationTimeout <= TimeSpan.Zero || terminationTimeout > TimeSpan.FromMinutes(1))
            throw new ArgumentOutOfRangeException(nameof(terminationTimeout));
        _terminationTimeout = terminationTimeout;
        _sip = new(this, CallProtocol.Sip); _h323 = new(this, CallProtocol.H323);
        worker.LineReceived += ReceiveLine;
        worker.GenerationEnded += (_, ended) =>
        {
            lock (_stateLock) { _started = false; _callProtocol = null; _selected = null; _terminal?.TrySetException(new IOException("OPAL worker exited before confirming call termination.")); }
            _sip.End(ended); _h323.End(ended);
        };
    }

    public IProtocolConsole Sip => _sip;
    public IProtocolConsole H323 => _h323;
    public int? ProcessId => (_worker as ProtocolConsoleProcess)?.ProcessId;
    public long ProcessGeneration => _worker.ProcessGeneration;

    private async Task StartAsync(CancellationToken token)
    {
        using var lifetime = CancellationTokenSource.CreateLinkedTokenSource(token, _retirement.Token);
        token = lifetime.Token;
        await WaitForGateAsync(token);
        try
        {
            if (_started && _generation == _worker.ProcessGeneration) return;
            await _worker.StartAsync(token);
            _started = true;
            lock (_stateLock) _generation = _worker.ProcessGeneration;
        }
        finally { _gate.Release(); }
    }

    public async Task SelectProtocolAsync(CallProtocol protocol, CancellationToken token)
    {
        using var lifetime = CancellationTokenSource.CreateLinkedTokenSource(token, _retirement.Token);
        token = lifetime.Token;
        await StartAsync(token);
        await WaitForGateAsync(token);
        try { await SelectLockedAsync(protocol, token); }
        finally { _gate.Release(); }
    }

    private async Task SelectLockedAsync(CallProtocol protocol, CancellationToken token)
    {
        lock (_stateLock)
        {
            if (_callProtocol is { } active && active != protocol) throw new InvalidOperationException("End the current call before changing protocol.");
            if (_callProtocol is not null || _selected == protocol) return;
        }
        await _worker.WriteLineAsync(protocol == CallProtocol.Sip ? "protocol sip" : "protocol h323", token);
        lock (_stateLock) _selected = protocol;
    }

    private async Task WriteAsync(CallProtocol protocol, string command, CancellationToken token)
    {
        using var lifetime = CancellationTokenSource.CreateLinkedTokenSource(token, _retirement.Token);
        token = lifetime.Token;
        ArgumentException.ThrowIfNullOrWhiteSpace(command);
        if (command.Contains('\r') || command.Contains('\n')) throw new ArgumentException("A console command must occupy one line.", nameof(command));
        await WaitForGateAsync(token);
        try
        {
            if (!_started) throw new InvalidOperationException("OPAL worker has not started.");
            // Retired surfaces belong to their originating call. The publisher resolves
            // their global epoch, so releasing them must not select the old protocol.
            if (!IsSurfaceRelease(command)) await SelectLockedAsync(protocol, token);
            lock (_stateLock)
            {
                if (_callProtocol is not null && command.StartsWith("aac-enabled ", StringComparison.Ordinal))
                    throw new InvalidOperationException("AAC requires an idle call engine.");
                if (command.StartsWith("call ", StringComparison.Ordinal))
                {
                    if (_callProtocol is not null) throw new InvalidOperationException("A call is already active or pending.");
                    _callProtocol = protocol;
                    _terminal = new(TaskCreationOptions.RunContinuationsAsynchronously);
                }
            }
            // A timed-out write may already have reached native code. Keep ownership
            // until actual terminal state or worker exit confirms the call has ended.
            await _worker.WriteLineAsync(command, token);
        }
        finally { _gate.Release(); }
    }

    private static bool IsSurfaceRelease(string command)
    {
        var separator = command.IndexOf(' ');
        if (separator < 0) return false;
        var verb = command[..separator];
        if (verb is not ("surface-released" or "surface-removed" or "local-surface-released" or "local-surface-removed")) return false;
        var value = command[(separator + 1)..];
        return ulong.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out var epoch)
            && epoch > 0 && value == epoch.ToString(CultureInfo.InvariantCulture);
    }

    private async Task AbortAsync(CallProtocol protocol, CancellationToken token)
    {
        Task? terminal;
        lock (_stateLock) terminal = _callProtocol == protocol ? _terminal?.Task : null;
        if (terminal is null) return;
        await WriteAsync(protocol, "hangup", token);
        await terminal.WaitAsync(_terminationTimeout, token);
    }

    private void ReceiveLine(object? sender, ProtocolConsoleLine line)
    {
        if (line.ProcessGeneration != _worker.ProcessGeneration) return;
        View target;
        lock (_stateLock)
        {
            if (_generation != line.ProcessGeneration)
            {
                _generation = line.ProcessGeneration; _sequence = 0; _selected = null; _callProtocol = null;
            }
            if (line.IsJsonShaped)
            {
                if (line.IsError) return;
                NativeJsonEvent? parsed;
                if (NativeJsonEvent.TryParse(line.Bytes.Span, CallProtocol.Sip, out parsed, out _)) target = _sip;
                else if (NativeJsonEvent.TryParse(line.Bytes.Span, CallProtocol.H323, out parsed, out _)) target = _h323;
                else return;
                if (parsed!.Sequence <= _sequence) return;
                _sequence = parsed.Sequence;
            }
            else
            {
                if (!line.TryGetLegacyText(out var text)) return;
                if (text == "video-startup-preferences-ready-v1") { _sip.Publish(line); _h323.Publish(line); return; }
                EngineEvent? state;
                if (ProtocolConsoleEventParser.TryParse(CallProtocol.H323, text, "unknown", out state)) target = _h323;
                else if (ProtocolConsoleEventParser.TryParse(CallProtocol.Sip, text, "unknown", out state)) target = _sip;
                else target = _selected == CallProtocol.H323 ? _h323 : _sip;
                if (state is IncomingCall or CallConnected)
                {
                    _callProtocol ??= target.Protocol;
                    _selected = _callProtocol;
                    _terminal ??= new(TaskCreationOptions.RunContinuationsAsynchronously);
                }
                else if (state is CallEnded or CallFailed && _callProtocol == target.Protocol)
                {
                    _callProtocol = null; _terminal?.TrySetResult(); _terminal = null;
                }
            }
        }
        target.Publish(line);
    }

    private ValueTask ReleaseAsync(bool firstRelease)
    {
        lock (_releaseLock)
        {
            if (firstRelease) _viewsReleased++;
            if (_viewsReleased != 2) return ValueTask.CompletedTask;
            if (_releaseTask is null || _releaseTask.IsFaulted) _releaseTask = ReleaseCoreAsync();
            return new(_releaseTask);
        }
    }

    private async Task ReleaseCoreAsync()
    {
        _retirement.Cancel();
        await WaitForGateAsync(CancellationToken.None);
        try { await _worker.DisposeAsync(); }
        finally { _worker.LineReceived -= ReceiveLine; _gate.Release(); }
    }

    private async Task WaitForGateAsync(CancellationToken token)
    {
        if (!await _gate.WaitAsync(ProtocolConsoleProcess.CommandTimeout, token))
            throw new TimeoutException("OPAL command gate did not become available within its deadline.");
    }

    private sealed class View(SharedOpalConsole owner, CallProtocol protocol) : IProtocolConsole
    {
        private bool _disposed;
        private int _releaseCounted;
        public CallProtocol Protocol => protocol;
        public long ProcessGeneration => owner._worker.ProcessGeneration;
        public event EventHandler<ProtocolConsoleLine>? LineReceived;
        public event EventHandler<ProtocolConsoleGenerationEnded>? GenerationEnded;
        public void ConfigureVideoStartup(SetVideoPreferences preferences) => owner._worker.ConfigureVideoStartup(preferences);
        public Task StartAsync(CancellationToken token) { ObjectDisposedException.ThrowIf(_disposed, this); return owner.StartAsync(token); }
        public Task PrepareDialAsync(string destination, CancellationToken token) => owner.SelectProtocolAsync(protocol, token);
        public Task AbortDialAsync(CancellationToken token) => owner.AbortAsync(protocol, token);
        public Task WriteLineAsync(string command, CancellationToken token) { ObjectDisposedException.ThrowIf(_disposed, this); return owner.WriteAsync(protocol, command, token); }
        public void Publish(ProtocolConsoleLine line) { if (!_disposed) LineReceived?.Invoke(this, line); }
        public void End(ProtocolConsoleGenerationEnded ended) { if (!_disposed) GenerationEnded?.Invoke(this, ended); }
        public ValueTask DisposeAsync() { var firstRelease = Interlocked.Exchange(ref _releaseCounted, 1) == 0; _disposed = true; return owner.ReleaseAsync(firstRelease); }
    }
}
