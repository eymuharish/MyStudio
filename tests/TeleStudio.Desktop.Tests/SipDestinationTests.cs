using TeleStudio.Contracts;
using TeleStudio.Presentation;
namespace TeleStudio.Desktop.Tests;
public sealed class SipDestinationTests
{
    [Theory]
    [InlineData("127.0.0.1:5121")]
    [InlineData("sip:user@10.136.51.25:5060")]
    [InlineData("sips:user@example.org;transport=tls")]
    public void Sip_display_model_accepts_direct_ports_and_account_addresses(string address)
    {
        var model = new MainViewModel(new Sink()) { SelectedProtocol = CallProtocol.Sip, Destination = address };
        Assert.True(model.CanDial); model.SelectedProtocol = CallProtocol.H323; Assert.False(model.CanDial);
    }
    private sealed class Sink : IEngineCommandSink { public Task SendAsync(EngineCommand command, CancellationToken token) => Task.CompletedTask; }
}
