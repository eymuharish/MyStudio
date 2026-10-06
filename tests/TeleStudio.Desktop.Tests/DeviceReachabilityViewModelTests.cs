using System.Net;
using TeleStudio.Desktop;

namespace TeleStudio.Desktop.Tests;

public sealed class DeviceReachabilityViewModelTests
{
    [Fact]
    public async Task Any_reply_reports_connected_in_both_languages()
    {
        var clock = new WindowClock();
        var sender = new TimedScriptedPingSender(clock, false, true, true);
        await using var model = new DeviceReachabilityViewModel(sender, clock,
            TimeSpan.Zero, TimeSpan.FromMilliseconds(200), TimeSpan.FromMilliseconds(30));

        await model.CheckAsync("sip:studio@192.0.2.7", true);
        Assert.Equal("متصل", model.StatusText);
        await model.CheckAsync("192.0.2.7", false);
        Assert.Equal("Connected", model.StatusText);
        Assert.Equal(14, sender.Attempts);
        Assert.Equal(TimeSpan.FromMilliseconds(400), clock.GetUtcNow() - DateTimeOffset.UnixEpoch);
    }

    [Fact]
    public async Task Early_success_does_not_complete_before_the_full_window()
    {
        var clock = new WindowClock();
        var sender = new ScriptedPingSender(true, true, true);
        await using var model = new DeviceReachabilityViewModel(sender, clock,
            TimeSpan.Zero, TimeSpan.FromMilliseconds(90), TimeSpan.FromMilliseconds(30));

        var check = model.CheckAsync("192.0.2.9", false);
        clock.Advance(TimeSpan.FromMilliseconds(25));

        Assert.False(check.IsCompleted);
        Assert.Equal("Checking", model.StatusText);
        clock.Advance(TimeSpan.FromMilliseconds(5));
        await clock.NextTimerCreated.WaitAsync(TimeSpan.FromSeconds(2));
        clock.Advance(TimeSpan.FromMilliseconds(30));
        await clock.NextTimerCreated.WaitAsync(TimeSpan.FromSeconds(2));
        clock.Advance(TimeSpan.FromMilliseconds(30));
        await check.WaitAsync(TimeSpan.FromSeconds(2));
        Assert.Equal("Connected", model.StatusText);
        Assert.Equal(TimeSpan.FromMilliseconds(90), clock.GetUtcNow() - DateTimeOffset.UnixEpoch);
    }

    [Fact]
    public async Task Partial_numeric_address_is_not_probed()
    {
        var sender = new ScriptedPingSender(true);
        await using var model = new DeviceReachabilityViewModel(sender, TimeProvider.System);

        await model.CheckAsync("10", true);

        Assert.Equal(string.Empty, model.StatusText);
        Assert.Equal(0, sender.Attempts);
    }

    [Fact]
    public async Task No_reply_until_total_deadline_reports_not_connected()
    {
        var clock = new WindowClock();
        var sender = new TimedOutPingSender(clock);
        await using var model = new DeviceReachabilityViewModel(sender, clock,
            TimeSpan.Zero, TimeSpan.FromMilliseconds(70), TimeSpan.FromMilliseconds(25));

        await model.CheckAsync("192.0.2.8", true);

        Assert.Equal("غير متصل", model.StatusText);
        Assert.Equal(3, sender.Attempts);
        Assert.Equal(TimeSpan.FromMilliseconds(70), clock.GetUtcNow() - DateTimeOffset.UnixEpoch);
    }

    [Fact]
    public async Task New_address_cancels_and_fences_late_positive_result()
    {
        var sender = new AddressPingSender();
        await using var model = new DeviceReachabilityViewModel(sender, TimeProvider.System,
            TimeSpan.Zero, TimeSpan.FromMilliseconds(100), TimeSpan.FromMilliseconds(30));

        var stale = model.CheckAsync("192.0.2.1", false);
        await Task.Delay(5);
        var current = model.CheckAsync("192.0.2.2", false);
        sender.Complete(IPAddress.Parse("192.0.2.1"), true);
        sender.Complete(IPAddress.Parse("192.0.2.2"), false);
        await Task.WhenAll(stale, current);

        Assert.Equal("Not connected", model.StatusText);
    }

    private sealed class WindowClock : TimeProvider
    {
        private readonly object _gate = new();
        private DateTimeOffset _now = DateTimeOffset.UnixEpoch;
        private readonly List<WindowTimer> _timers = [];
        private TaskCompletionSource _timerCreated = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public Task NextTimerCreated { get { lock (_gate) return _timerCreated.Task; } }
        public override DateTimeOffset GetUtcNow() { lock (_gate) return _now; }
        public override ITimer CreateTimer(TimerCallback callback, object? state, TimeSpan dueTime, TimeSpan period)
        {
            lock (_gate)
            {
                var timer = new WindowTimer(this, callback, state);
                _timers.Add(timer);
                timer.Change(dueTime, period);
                _timerCreated.TrySetResult();
                return timer;
            }
        }
        public void Advance(TimeSpan duration)
        {
            WindowTimer[] due;
            lock (_gate)
            {
                _now += duration;
                due = _timers.Where(timer => timer.Due is { } time && time <= _now).ToArray();
                if (due.Length != 0) _timerCreated = new(TaskCreationOptions.RunContinuationsAsynchronously);
            }
            foreach (var timer in due) timer.Fire();
        }
        private sealed class WindowTimer(WindowClock clock, TimerCallback callback, object? state) : ITimer
        {
            public DateTimeOffset? Due { get; private set; }
            public bool Change(TimeSpan dueTime, TimeSpan period)
            { lock (clock._gate) Due = dueTime == Timeout.InfiniteTimeSpan ? null : clock._now + dueTime; return true; }
            public void Fire() { lock (clock._gate) { if (Due is null) return; Due = null; } callback(state); }
            public void Dispose() { lock (clock._gate) Due = null; }
            public ValueTask DisposeAsync() { Dispose(); return ValueTask.CompletedTask; }
        }
    }

    private sealed class TimedOutPingSender(WindowClock clock) : IReachabilityPingSender
    {
        public int Attempts { get; private set; }
        public Task<bool> SendAsync(IPAddress address, TimeSpan timeout, CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            Attempts++;
            clock.Advance(timeout);
            return Task.FromResult(false);
        }
    }

    private sealed class TimedScriptedPingSender(WindowClock clock, params bool[] results) : IReachabilityPingSender
    {
        public int Attempts { get; private set; }
        public Task<bool> SendAsync(IPAddress address, TimeSpan timeout, CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            clock.Advance(timeout);
            return Task.FromResult(results[Math.Min(Attempts++, results.Length - 1)]);
        }
    }

    private sealed class ScriptedPingSender(params bool[] results) : IReachabilityPingSender
    {
        private int _index;
        public int Attempts => _index;
        public Task<bool> SendAsync(IPAddress address, TimeSpan timeout, CancellationToken cancellationToken)
        {
            var index = Interlocked.Increment(ref _index) - 1;
            return Task.FromResult(results.Length != 0 && results[Math.Min(index, results.Length - 1)]);
        }
    }

    private sealed class AddressPingSender : IReachabilityPingSender
    {
        private readonly Dictionary<IPAddress, TaskCompletionSource<bool>> _pending = [];
        public Task<bool> SendAsync(IPAddress address, TimeSpan timeout, CancellationToken cancellationToken)
        {
            var source = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
            if (_pending.TryGetValue(address, out var existing)) return existing.Task;
            _pending[address] = source; return source.Task;
        }
        public void Complete(IPAddress address, bool result) => _pending[address].TrySetResult(result);
    }
}
