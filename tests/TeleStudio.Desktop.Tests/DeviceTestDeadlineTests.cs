using TeleStudio.Desktop;
namespace TeleStudio.Desktop.Tests;
public sealed class DeviceTestDeadlineTests
{
    private static readonly DateTimeOffset Cutoff = DateTimeOffset.Parse("2026-10-04T03:55:00Z");
    [Theory]
    [InlineData("10.136.51.25")]
    [InlineData("sip:user@10.136.51.11:5060")]
    [InlineData("h323:10.136.203.16")]
    [InlineData("sip:user@example.org")]
    public void Physical_calls_cannot_start_at_or_after_cutoff(string destination)
    {
        var policy = new DeviceTestDeadline(Cutoff);
        Assert.True(policy.BlockStart(destination, Cutoff));
        Assert.False(policy.BlockStart(destination, Cutoff.AddMinutes(-10)));
        Assert.True(policy.EndCall(destination, Cutoff.AddSeconds(-45)));
    }
    [Theory]
    [InlineData("127.0.0.1:5121")]
    [InlineData("sip:user@127.0.0.2:5060")]
    [InlineData("sip:user@[::1]:5060")]
    [InlineData("localhost")]
    [InlineData("ip$127.0.0.1:1720")]
    public void Localhost_regressions_remain_allowed_after_cutoff(string destination)
    {
        var policy = new DeviceTestDeadline(Cutoff);
        Assert.False(policy.BlockStart(destination, Cutoff.AddHours(2)));
        Assert.False(policy.EndCall(destination, Cutoff.AddHours(2)));
    }
    [Theory]
    [InlineData(TeleStudio.Contracts.CallProtocol.Sip)]
    [InlineData(TeleStudio.Contracts.CallProtocol.H323)]
    public async Task Managed_boundary_blocks_physical_invite_and_rejects_incoming_after_deadline(TeleStudio.Contracts.CallProtocol protocol)
    {
        var h323 = new ConsoleFixture(TeleStudio.Contracts.CallProtocol.H323); var sip = new ConsoleFixture(TeleStudio.Contracts.CallProtocol.Sip);
        await using var sink = new NativeEngineCommandSink(h323, sip, new Clock(Cutoff), Cutoff);
        var events = new List<TeleStudio.Contracts.EngineEvent>(); sink.EventReceived += (_, value) => events.Add(value);
        await sink.StartAsync(default);
        await Assert.ThrowsAsync<InvalidOperationException>(() => sink.SendAsync(new TeleStudio.Contracts.Dial("10.136.51.25", protocol, "MyStudio"), default));
        Assert.Empty(h323.Commands); Assert.Empty(sip.Commands);
        var console = protocol == TeleStudio.Contracts.CallProtocol.Sip ? sip : h323;
        console.Emit(protocol == TeleStudio.Contracts.CallProtocol.Sip ? "incoming call from 10.136.51.25; enter answer or reject" : "incoming-call remote=\"ip$10.136.51.25:1720\" action=answer");
        Assert.Contains("reject", console.Commands); Assert.DoesNotContain(events, x => x is TeleStudio.Contracts.IncomingCall);
    }
    [Theory]
    [InlineData(TeleStudio.Contracts.CallProtocol.Sip, "10.136.51.25", true)]
    [InlineData(TeleStudio.Contracts.CallProtocol.H323, "10.136.51.25", true)]
    [InlineData(TeleStudio.Contracts.CallProtocol.Sip, "127.0.0.1:5121", false)]
    [InlineData(TeleStudio.Contracts.CallProtocol.H323, "127.0.0.1", false)]
    public async Task Deadline_timer_gracefully_ends_physical_calls_and_preserves_local_calls(TeleStudio.Contracts.CallProtocol protocol, string destination, bool shouldEnd)
    {
        var clock = new Clock(Cutoff.AddSeconds(-46)); var h323 = new ConsoleFixture(TeleStudio.Contracts.CallProtocol.H323); var sip = new ConsoleFixture(TeleStudio.Contracts.CallProtocol.Sip);
        await using var sink = new NativeEngineCommandSink(h323, sip, clock, Cutoff); await sink.StartAsync(default);
        await sink.SendAsync(new TeleStudio.Contracts.Dial(destination, protocol, "MyStudio"), default);
        clock.Advance(TimeSpan.FromSeconds(2));
        Assert.Equal(shouldEnd, (protocol == TeleStudio.Contracts.CallProtocol.Sip ? sip : h323).Commands.Contains("hangup"));
    }
    private sealed class Clock(DateTimeOffset now) : TimeProvider
    {
        private DateTimeOffset current = now;
        private readonly List<ClockTimer> timers = [];
        public override DateTimeOffset GetUtcNow() => current;
        public override ITimer CreateTimer(TimerCallback callback, object? state, TimeSpan dueTime, TimeSpan period)
        { var timer = new ClockTimer(this, callback, state); timers.Add(timer); timer.Change(dueTime, period); return timer; }
        public void Advance(TimeSpan amount) { current += amount; foreach (var timer in timers.ToArray()) timer.Fire(); }
        private sealed class ClockTimer(Clock clock, TimerCallback callback, object? state) : ITimer
        {
            private DateTimeOffset? due;
            public bool Change(TimeSpan dueTime, TimeSpan period) { due = dueTime == Timeout.InfiniteTimeSpan ? null : clock.current + dueTime; return true; }
            public void Fire() { if (due is { } at && clock.current >= at) { due = null; callback(state); } }
            public void Dispose() => due = null;
            public ValueTask DisposeAsync() { Dispose(); return ValueTask.CompletedTask; }
        }
    }
    private sealed class ConsoleFixture(TeleStudio.Contracts.CallProtocol protocol) : IProtocolConsole
    {
        public TeleStudio.Contracts.CallProtocol Protocol => protocol;
        public long ProcessGeneration => 1;
        public List<string> Commands { get; } = [];
        public event EventHandler<ProtocolConsoleLine>? LineReceived;
        public event EventHandler<ProtocolConsoleGenerationEnded>? GenerationEnded { add { } remove { } }
        public Task StartAsync(CancellationToken token) => Task.CompletedTask;
        public Task WriteLineAsync(string command, CancellationToken token) { Commands.Add(command); return Task.CompletedTask; }
        public void Emit(string text) => LineReceived?.Invoke(this, new(text, false, 1));
        public ValueTask DisposeAsync() => ValueTask.CompletedTask;
    }
}
