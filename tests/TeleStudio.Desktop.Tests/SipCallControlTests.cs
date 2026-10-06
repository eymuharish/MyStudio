using TeleStudio.Contracts;

namespace TeleStudio.Desktop.Tests;

public sealed class SipCallControlTests
{
    [Fact]
    public void Confirmed_hold_event_is_parsed()
    {
        Assert.True(NativeJsonEvent.TryParse("{\"schema\":1,\"type\":\"call-hold-changed\",\"sequence\":1,\"protocol\":\"sip\",\"held\":true}\n"u8,
            CallProtocol.Sip, out var value, out var reason), reason);
        Assert.Equal("CallHoldChanged", value!.Event.GetType().Name);
    }

    [Fact]
    public void Capabilities_accept_optional_hold_and_dtmf_without_changing_legacy_records()
    {
        Assert.True(NativeJsonEvent.TryParse("{\"schema\":1,\"type\":\"media-ready\",\"sequence\":1,\"protocol\":\"sip\",\"sourceControl\":false,\"muteControl\":true,\"stats\":true,\"remoteSurface\":false,\"audioControl\":true,\"recording\":false,\"systemAudioControl\":false,\"roomMixerControl\":false,\"videoControl\":false,\"holdControl\":true,\"dtmfControl\":true}\n"u8,
            CallProtocol.Sip, out _, out var reason), reason);
    }

    [Theory]
    [InlineData("")]
    [InlineData("1\nhangup")]
    [InlineData("e")]
    public void Dtmf_rejects_invalid_or_command_injection_digits(string digits)
    {
        Assert.Throws<ArgumentException>(() => new SendDtmf(digits));
    }

    [Fact]
    public void Content_channel_state_parses_negotiated_h239_and_fallback_distinctly()
    {
        Assert.True(NativeJsonEvent.TryParse("{\"schema\":1,\"type\":\"content-sharing-state\",\"sequence\":1,\"protocol\":\"h323\",\"mode\":\"h239\",\"active\":true,\"detail\":\"separate negotiated channel\"}\n"u8,
            CallProtocol.H323, out var value, out var reason), reason);
        Assert.Equal("h239", Assert.IsType<ContentSharingStateChanged>(value!.Event).Mode);
    }
}
