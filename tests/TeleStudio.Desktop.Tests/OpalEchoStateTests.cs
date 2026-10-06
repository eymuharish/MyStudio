using TeleStudio.Contracts;
using TeleStudio.Desktop;
namespace TeleStudio.Desktop.Tests;
public sealed class OpalEchoStateTests
{
    [Theory]
    [InlineData(CallProtocol.Sip)]
    [InlineData(CallProtocol.H323)]
    public async Task Worker_echo_snapshots_reach_the_engine_and_only_fresh_acknowledgments_change_state(CallProtocol protocol)
    {
        var worker = new SharedOpalConsoleTests.Worker();
        await using var sink = new OpalEngineCommandSink(new SharedOpalConsole(worker), EngineFamily.Cisco);
        await sink.StartAsync(default);
        await sink.ActivateProtocolAsync(protocol, default);
        var name = protocol == CallProtocol.Sip ? "sip" : "h323";
        void Snapshot(int sequence, bool enabled) => worker.Emit($"{{\"schema\":1,\"type\":\"echo-cancellation-state\",\"sequence\":{sequence},\"protocol\":\"{name}\",\"available\":true,\"enabled\":{enabled.ToString().ToLowerInvariant()}}}");
        Snapshot(100, true);
        Assert.Equal(new EchoCancellationChanged(true, true), sink.EchoState);
        await sink.SendAsync(new SetEchoCancellation(false), default);
        Assert.Equal("echo-cancellation off", worker.Commands.Last());
        Assert.True(sink.EchoState.Enabled);
        Snapshot(100, false);
        Assert.True(sink.EchoState.Enabled);
        Snapshot(101, false);
        Assert.False(sink.EchoState.Enabled);
    }

    [Fact]
    public void Echo_status_is_confirmed_from_the_worker()
    {
        var wire = "{\"schema\":1,\"type\":\"echo-cancellation-state\",\"sequence\":7,\"protocol\":\"sip\",\"available\":true,\"enabled\":true}\n"u8;
        Assert.True(NativeJsonEvent.TryParse(wire, CallProtocol.Sip, out var parsed, out var reason), reason);
        Assert.Equal(new EchoCancellationChanged(true, true), parsed!.Event);
    }
    [Fact]
    public void Unavailable_echo_processing_cannot_claim_to_be_enabled()
    {
        var wire = "{\"schema\":1,\"type\":\"echo-cancellation-state\",\"sequence\":7,\"protocol\":\"sip\",\"available\":false,\"enabled\":true}\n"u8;
        Assert.False(NativeJsonEvent.TryParse(wire, CallProtocol.Sip, out _, out _));
    }
}
