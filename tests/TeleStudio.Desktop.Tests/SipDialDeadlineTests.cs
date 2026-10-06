using TeleStudio.Contracts;

namespace TeleStudio.Desktop.Tests;

public sealed class SipDialDeadlineTests
{
    [Fact]
    public async Task Missing_confirmation_emits_timeout_and_allows_retry()
    {
        var sip = new ConsoleStub(CallProtocol.Sip);
        var clock = new ManualClock();
        await using var sink = new NativeEngineCommandSink(new ConsoleStub(CallProtocol.H323), sip, clock);
        var failure = new TaskCompletionSource<CallFailed>(TaskCreationOptions.RunContinuationsAsynchronously);
        sink.EventReceived += (_, e) => { if (e is CallFailed failed) failure.TrySetResult(failed); };
        await sink.StartAsync(CancellationToken.None);
        await sink.SendAsync(new Dial("10.136.51.25", CallProtocol.Sip, "Essa"), CancellationToken.None);
        clock.Advance(TimeSpan.FromSeconds(29));
        Assert.False(failure.Task.IsCompleted);
        clock.Advance(TimeSpan.FromSeconds(1));
        var result = await failure.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal("sip-call-timeout", result.ErrorCode);
        await sink.SendAsync(new Dial("10.136.203.13", CallProtocol.Sip, "Essa"), CancellationToken.None);
        Assert.Equal("call 10.136.203.13", sip.Commands.Last());
    }

    [Theory]
    [InlineData("call 0: CONFIRMED (OK)")]
    [InlineData("call 0: DISCONNECTED (Busy)")]
    public async Task Terminal_native_events_cancel_the_deadline(string nativeEvent)
    {
        var clock = new ManualClock();
        var sip = new ConsoleStub(CallProtocol.Sip);
        await using var sink = new NativeEngineCommandSink(new ConsoleStub(CallProtocol.H323), sip, clock);
        var events = new List<EngineEvent>();
        sink.EventReceived += (_, e) => events.Add(e);
        await sink.StartAsync(CancellationToken.None);
        await sink.SendAsync(new Dial("10.0.0.1", CallProtocol.Sip, "Essa"), CancellationToken.None);
        sip.Emit(nativeEvent);
        clock.Advance(TimeSpan.FromSeconds(60));
        Assert.DoesNotContain(events, e => e is CallFailed { ErrorCode: "sip-call-timeout" });
        Assert.DoesNotContain("abort", sip.Commands);
    }

    [Fact]
    public async Task Cancel_pending_call_retires_it_and_ignores_old_confirmation()
    {
        var clock = new ManualClock();
        var sip = new ConsoleStub(CallProtocol.Sip);
        await using var sink = new NativeEngineCommandSink(new ConsoleStub(CallProtocol.H323), sip, clock);
        var events = new List<EngineEvent>();
        sink.EventReceived += (_, e) => events.Add(e);
        await sink.StartAsync(CancellationToken.None);
        await sink.SendAsync(new Dial("10.0.0.1", CallProtocol.Sip, "Essa"), CancellationToken.None);
        var oldGeneration = sip.ProcessGeneration;
        await sink.SendAsync(new HangUp(), CancellationToken.None);
        await sink.SendAsync(new Dial("10.0.0.2", CallProtocol.Sip, "Essa"), CancellationToken.None);
        sip.Emit("call 0: CONFIRMED (OK)", oldGeneration);
        Assert.DoesNotContain(events, e => e is CallConnected);
        sip.Emit("call 0: CONFIRMED (OK)");
        clock.Advance(TimeSpan.FromMinutes(1));
        Assert.Single(events.OfType<CallConnected>());
        Assert.Empty(events.OfType<CallFailed>());
        Assert.Single(events.OfType<CallEnded>());
    }

    [Fact]
    public async Task Deadline_includes_network_preparation_and_cancels_stalled_prepare()
    {
        var clock = new ManualClock();
        var sip = new ConsoleStub(CallProtocol.Sip) { StallPrepare = true };
        await using var sink = new NativeEngineCommandSink(new ConsoleStub(CallProtocol.H323), sip, clock);
        var failure = new TaskCompletionSource<CallFailed>(TaskCreationOptions.RunContinuationsAsynchronously);
        sink.EventReceived += (_, e) => { if (e is CallFailed failed) failure.TrySetResult(failed); };
        await sink.StartAsync(CancellationToken.None);
        var dial = sink.SendAsync(new Dial("10.0.0.1", CallProtocol.Sip, "Essa"), CancellationToken.None);
        clock.Advance(TimeSpan.FromSeconds(30));
        await dial.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal("sip-call-timeout", (await failure.Task.WaitAsync(TimeSpan.FromSeconds(5))).ErrorCode);
        Assert.DoesNotContain(sip.Commands, c => c.StartsWith("call "));
    }

    [Fact]
    public async Task Process_exit_ends_pending_ui_state_instead_of_silently_clearing_engine()
    {
        var sip = new ConsoleStub(CallProtocol.Sip);
        await using var sink = new NativeEngineCommandSink(new ConsoleStub(CallProtocol.H323), sip);
        var events = new List<EngineEvent>();
        sink.EventReceived += (_, e) => events.Add(e);
        await sink.StartAsync(CancellationToken.None);
        await sink.SendAsync(new Dial("10.0.0.1", CallProtocol.Sip, "Essa"), CancellationToken.None);
        sip.Exit();
        Assert.Equal("sip-engine-exited", Assert.Single(events.OfType<CallFailed>()).ErrorCode);
    }

    [Fact]
    public async Task Disposal_cancels_stalled_prepare_before_disposing_owned_resources()
    {
        var sip = new ConsoleStub(CallProtocol.Sip) { StallPrepare = true };
        var sink = new NativeEngineCommandSink(new ConsoleStub(CallProtocol.H323), sip);
        var events = new List<EngineEvent>();
        sink.EventReceived += (_, e) => events.Add(e);
        await sink.StartAsync(CancellationToken.None);
        var dial = sink.SendAsync(new Dial("10.0.0.1", CallProtocol.Sip, "Essa"), CancellationToken.None);
        await sink.DisposeAsync().AsTask().WaitAsync(TimeSpan.FromSeconds(2));
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => dial.WaitAsync(TimeSpan.FromSeconds(2)));
        sip.Emit("call 0: CONFIRMED (OK)");
        Assert.Empty(events);
    }

    [Fact]
    public async Task Surface_acknowledgements_reject_disposal_before_accessing_console_owners()
    {
        var sink = new NativeEngineCommandSink(new ConsoleStub(CallProtocol.H323), new ConsoleStub(CallProtocol.Sip));
        await sink.DisposeAsync();
        await Assert.ThrowsAsync<ObjectDisposedException>(() => sink.SendSurfaceAcknowledgementAsync(
            SurfaceAcknowledgement.Removed, 1, CancellationToken.None));
        await Assert.ThrowsAsync<ObjectDisposedException>(() => sink.SendLocalSurfaceAcknowledgementAsync(
            SurfaceAcknowledgement.Removed, 1, CancellationToken.None));
    }

    internal sealed class ConsoleStub(CallProtocol protocol) : IProtocolConsole
    {
        public CallProtocol Protocol => protocol;
        public long ProcessGeneration { get; private set; } = 1;
        public List<string> Commands { get; } = [];
        public bool StallPrepare { get; set; }
        public event EventHandler<ProtocolConsoleLine>? LineReceived;
        public event EventHandler<ProtocolConsoleGenerationEnded>? GenerationEnded;
        public Task StartAsync(CancellationToken cancellationToken) => Task.CompletedTask;
        public Task PrepareDialAsync(string destination, CancellationToken cancellationToken) =>
            StallPrepare ? Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken) : Task.CompletedTask;
        public Task WriteLineAsync(string command, CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            Commands.Add(command);
            return Task.CompletedTask;
        }
        public Task AbortDialAsync(CancellationToken cancellationToken)
        {
            Commands.Add("abort");
            ProcessGeneration++;
            return Task.CompletedTask;
        }
        public void Emit(string text, long? generation = null) => LineReceived?.Invoke(this,
            new ProtocolConsoleLine(text, false, generation ?? ProcessGeneration));
        public void Exit() => GenerationEnded?.Invoke(this, new(ProcessGeneration));
        public ValueTask DisposeAsync() => ValueTask.CompletedTask;
    }

    internal sealed class ManualClock : TimeProvider
    {
        private TimeSpan _now;
        private readonly List<ManualTimer> _timers = [];
        public override ITimer CreateTimer(TimerCallback callback, object? state, TimeSpan dueTime, TimeSpan period)
        {
            var timer = new ManualTimer(this, callback, state);
            _timers.Add(timer);
            timer.Change(dueTime, period);
            return timer;
        }
        public void Advance(TimeSpan amount)
        {
            _now += amount;
            foreach (var timer in _timers.ToArray()) timer.Fire(_now);
        }
        private sealed class ManualTimer(ManualClock clock, TimerCallback callback, object? state) : ITimer
        {
            private TimeSpan? _due;
            public bool Change(TimeSpan dueTime, TimeSpan period)
            { _due = dueTime == Timeout.InfiniteTimeSpan ? null : clock._now + dueTime; return true; }
            public void Fire(TimeSpan now)
            { if (_due is { } due && now >= due) { _due = null; callback(state); } }
            public void Dispose() => _due = null;
            public ValueTask DisposeAsync() { Dispose(); return ValueTask.CompletedTask; }
        }
    }
}
