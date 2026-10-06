using System.ComponentModel;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Runtime.CompilerServices;

namespace TeleStudio.Desktop;

public interface IReachabilityPingSender
{
    Task<bool> SendAsync(IPAddress address, TimeSpan timeout, CancellationToken cancellationToken);
}

public sealed class IcmpReachabilityPingSender : IReachabilityPingSender
{
    public async Task<bool> SendAsync(IPAddress address, TimeSpan timeout, CancellationToken cancellationToken)
    {
        using var ping = new Ping();
        try
        {
            var milliseconds = Math.Max(1, checked((int)Math.Ceiling(timeout.TotalMilliseconds)));
            var reply = await ping.SendPingAsync(address, milliseconds).WaitAsync(cancellationToken);
            return reply.Status == IPStatus.Success;
        }
        catch (PingException) { return false; }
        catch (SocketException) { return false; }
    }
}

public sealed class DeviceReachabilityViewModel : INotifyPropertyChanged, IAsyncDisposable
{
    private readonly IReachabilityPingSender _sender;
    private readonly TimeProvider _timeProvider;
    private readonly TimeSpan _debounce, _window, _attemptTimeout;
    private CancellationTokenSource? _active;
    private long _generation;
    private string _statusText = string.Empty;

    public DeviceReachabilityViewModel(IReachabilityPingSender? sender = null, TimeProvider? timeProvider = null,
        TimeSpan? debounce = null, TimeSpan? window = null, TimeSpan? attemptTimeout = null)
    {
        _sender = sender ?? new IcmpReachabilityPingSender();
        _timeProvider = timeProvider ?? TimeProvider.System;
        _debounce = debounce ?? TimeSpan.FromMilliseconds(400);
        _window = window ?? TimeSpan.FromSeconds(5);
        _attemptTimeout = attemptTimeout ?? TimeSpan.FromSeconds(1);
    }

    public string StatusText { get => _statusText; private set { if (_statusText == value) return; _statusText = value; PropertyChanged?.Invoke(this, new(nameof(StatusText))); } }
    public event PropertyChangedEventHandler? PropertyChanged;

    public async Task CheckAsync(string? destination, bool arabic)
    {
        var generation = Interlocked.Increment(ref _generation);
        var next = new CancellationTokenSource();
        var token = next.Token;
        var prior = Interlocked.Exchange(ref _active, next);
        prior?.Cancel(); prior?.Dispose();
        if (!TryExtractIpv4(destination, out var address)) { StatusText = string.Empty; return; }
        StatusText = arabic ? "جارٍ الفحص" : "Checking";
        try
        {
            await Task.Delay(_debounce, _timeProvider, token);
            var deadline = _timeProvider.GetUtcNow() + _window;
            var anyReply = false;
            while (_timeProvider.GetUtcNow() < deadline)
            {
                var remaining = deadline - _timeProvider.GetUtcNow();
                var budget = remaining < _attemptTimeout ? remaining : _attemptTimeout;
                var attemptStarted = _timeProvider.GetUtcNow();
                try
                {
                    anyReply |= await _sender.SendAsync(address, budget, token)
                        .WaitAsync(budget, _timeProvider, token);
                }
                catch (TimeoutException) { }
                var unusedBudget = budget - (_timeProvider.GetUtcNow() - attemptStarted);
                if (unusedBudget > TimeSpan.Zero) await Task.Delay(unusedBudget, _timeProvider, token);
            }
            if (generation == Volatile.Read(ref _generation))
                StatusText = anyReply ? arabic ? "متصل" : "Connected" : arabic ? "غير متصل" : "Not connected";
        }
        catch (OperationCanceledException) when (token.IsCancellationRequested) { }
        catch
        {
            if (generation == Volatile.Read(ref _generation)) StatusText = string.Empty;
        }
    }

    internal static bool TryExtractIpv4(string? destination, out IPAddress address)
    {
        var text = destination?.Trim() ?? string.Empty;
        if (text.StartsWith("#sip:@", StringComparison.OrdinalIgnoreCase)) text = text[6..];
        if (text.StartsWith("sip:", StringComparison.OrdinalIgnoreCase)) text = text[4..];
        var at = text.LastIndexOf('@'); if (at >= 0) text = text[(at + 1)..];
        var colon = text.LastIndexOf(':'); if (colon > 0 && text[(colon + 1)..].All(char.IsDigit)) text = text[..colon];
        var octets = text.Split('.');
        if (octets.Length != 4 || octets.Any(part => part.Length == 0 || !part.All(char.IsDigit) ||
            !byte.TryParse(part, out _))) { address = IPAddress.None; return false; }
        return IPAddress.TryParse(text, out address!) && address.AddressFamily == AddressFamily.InterNetwork;
    }

    public ValueTask DisposeAsync()
    {
        Interlocked.Increment(ref _generation);
        var active = Interlocked.Exchange(ref _active, null); active?.Cancel(); active?.Dispose();
        return ValueTask.CompletedTask;
    }
}
