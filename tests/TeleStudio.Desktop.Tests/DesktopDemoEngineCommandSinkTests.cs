using TeleStudio.Contracts;
using TeleStudio.Desktop;

namespace TeleStudio.Desktop.Tests;

public sealed class DesktopDemoEngineCommandSinkTests
{
    [Fact]
    public async Task Dial_reports_a_connected_camera_session()
    {
        var sink = new DesktopDemoEngineCommandSink(TimeSpan.Zero);
        var events = new List<EngineEvent>();
        sink.EventReceived += (_, engineEvent) => events.Add(engineEvent);

        await sink.SendAsync(new Dial("10.136.51.25", CallProtocol.H323, "EssaStudio"), CancellationToken.None);

        Assert.Equal(new CallConnected("10.136.51.25", CallProtocol.H323, 1920, 1080, 30), Assert.Single(events));
    }

    [Fact]
    public async Task Hang_up_cancels_a_pending_demo_dial()
    {
        var sink = new DesktopDemoEngineCommandSink(TimeSpan.FromMilliseconds(50));
        var events = new List<EngineEvent>();
        sink.EventReceived += (_, engineEvent) => events.Add(engineEvent);

        var dial = sink.SendAsync(new Dial("10.136.51.25", CallProtocol.H323, "EssaStudio"), CancellationToken.None);
        await sink.SendAsync(new HangUp(), CancellationToken.None);
        await dial;

        Assert.DoesNotContain(events, engineEvent => engineEvent is CallConnected);
        Assert.IsType<CallEnding>(events[0]);
        Assert.IsType<CallEnded>(events[^1]);
    }
}
