using System.Text;
using TeleStudio.Contracts;

namespace TeleStudio.Desktop.Tests;

public sealed class OpalAvailabilityTests
{
    [Theory]
    [InlineData(CallProtocol.Sip, "sip")]
    [InlineData(CallProtocol.H323, "h323")]
    public void Opal_health_counters_are_accepted_for_both_actual_protocols(CallProtocol protocol, string name)
    {
        var wire = Encoding.UTF8.GetBytes($"{{\"schema\":1,\"type\":\"opal-connection-health\",\"sequence\":7,\"protocol\":\"{name}\",\"audioTxKbps\":null,\"audioRxKbps\":null,\"videoTxKbps\":null,\"videoRxKbps\":null,\"lossPercent\":null,\"jitterBufferMs\":null,\"roundTripMs\":null,\"capturedSamples\":48000,\"decodedAudioSamples\":16000,\"sentFrames\":30,\"receivedFrames\":25,\"decodedNonzeroSamples\":12800,\"deviceSubmittedNonzeroSamples\":38400}}\n");
        Assert.True(NativeJsonEvent.TryParse(wire, protocol, out var parsed, out var reason), reason);
        var health = Assert.IsType<SipConnectionHealthChanged>(parsed!.Event);
        Assert.Equal(16000, health.DecodedAudioSamples);
        Assert.Equal(38400, health.DeviceSubmittedNonzeroSamples);
        Assert.Equal(protocol, parsed.Protocol);
    }

    [Theory]
    [InlineData(CallProtocol.Sip, "sip")]
    [InlineData(CallProtocol.H323, "h323")]
    public void Plugin_availability_is_an_idle_event_with_actual_protocol(CallProtocol protocol, string name)
    {
        var wire = Encoding.UTF8.GetBytes($"{{\"schema\":1,\"type\":\"aac-availability\",\"sequence\":1,\"protocol\":\"{name}\",\"available\":false,\"detail\":\"AAC plugin unavailable\"}}\n");
        Assert.True(NativeJsonEvent.TryParse(wire, protocol, out var parsed, out var reason), reason);
        Assert.Equal("AacAvailabilityChanged", parsed!.Event.GetType().Name);
    }
}
