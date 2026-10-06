using System.Text;
using TeleStudio.Contracts;
using TeleStudio.Desktop;
namespace TeleStudio.Desktop.Tests;
public sealed class SipSessionTimerEventTests
{
    [Fact] public void Negotiated_session_timer_is_preserved_in_support_events()
    {
        var wire = "{\"schema\":1,\"type\":\"sip-session-timer\",\"sequence\":7,\"protocol\":\"sip\",\"sessionIntervalSeconds\":90,\"refreshAfterSeconds\":45,\"localRefresher\":true}\n"u8;
        Assert.True(NativeJsonEvent.TryParse(wire, CallProtocol.Sip, out var parsed, out var reason), reason);
        Assert.Equal("SipSessionTimerChanged", parsed!.Event.GetType().Name);
        var report = OpalSupportReport.Build(new {}, [parsed.Event], [], "", "");
        Assert.Contains("90", report); Assert.Contains("45", report);
    }
    [Theory]
    [InlineData(89,44,true)]
    [InlineData(90,90,true)]
    [InlineData(90,45,false)]
    public void Invalid_timer_diagnostics_are_rejected(int period,int after,bool local)
    {
        var wire = Encoding.UTF8.GetBytes($"{{\"schema\":1,\"type\":\"sip-session-timer\",\"sequence\":7,\"protocol\":\"sip\",\"sessionIntervalSeconds\":{period},\"refreshAfterSeconds\":{after},\"localRefresher\":{local.ToString().ToLowerInvariant()}}}\n");
        Assert.False(NativeJsonEvent.TryParse(wire, CallProtocol.Sip, out _, out _));
    }
}
