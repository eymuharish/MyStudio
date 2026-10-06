using TeleStudio.Contracts;

namespace TeleStudio.Desktop.Tests;

public sealed class SipDiagnosticsTests
{
    [Fact]
    public void Playback_health_preserves_unknown_network_measurements()
    {
        Assert.True(NativeJsonEvent.TryParse("{\"schema\":1,\"type\":\"sip-connection-health\",\"sequence\":1,\"protocol\":\"sip\",\"audioTxKbps\":64,\"audioRxKbps\":64,\"videoTxKbps\":null,\"videoRxKbps\":null,\"lossPercent\":null,\"jitterBufferMs\":100,\"roundTripMs\":null,\"capturedSamples\":16000,\"decodedAudioSamples\":16000,\"sentFrames\":0,\"receivedFrames\":0,\"decodedNonzeroSamples\":1000,\"deviceSubmittedNonzeroSamples\":1000}\n"u8,
            CallProtocol.Sip, out var value, out var reason), reason);
        Assert.Equal("SipConnectionHealthChanged", value!.Event.GetType().Name);
    }
    [Fact]
    public void Registration_event_is_parsed()
    {
        Assert.True(NativeJsonEvent.TryParse("{\"schema\":1,\"type\":\"sip-registration-state\",\"sequence\":1,\"protocol\":\"sip\",\"state\":\"ok\",\"detail\":\"registered\"}\n"u8,
            CallProtocol.Sip, out _, out var reason), reason);
    }
    [Fact]
    public void Child_account_environment_removes_inherited_password_when_registration_disabled()
    {
        var start = new System.Diagnostics.ProcessStartInfo();
        start.Environment["ESSASTUDIO_SIP_PASSWORD"] = "prior-value";
        ProtocolConsoleProcess.ApplyWorkerEnvironment(start, new Dictionary<string,string>());
        Assert.False(start.Environment.ContainsKey("ESSASTUDIO_SIP_PASSWORD"));
    }
}
