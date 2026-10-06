using System.Text;
using System.Text.Json;
using TeleStudio.Contracts;

namespace TeleStudio.Desktop.Tests;

public sealed class OpalVideoDiagnosticsTests
{
    internal const string Wire = """
        {"schema":1,"type":"opal-video-transport","sequence":5,"protocol":"sip","sessionId":2,"direction":"send","role":"main","codec":"H.264","payloadType":102,"width":1280,"height":720,"packetizationMode":0,"profileLevelId":"42e01f","localRtp":"udp$10.46.250.176:43002","remoteRtp":"udp$10.136.51.11:49154","packetsSent":240,"octetsSent":192000,"markersSent":10,"packetsReceived":0,"octetsReceived":0,"remotePacketsLost":null,"senderSsrc":12345}
        """;

    [Theory]
    [InlineData(CallProtocol.Sip, "sip")]
    [InlineData(CallProtocol.H323, "h323")]
    public void Support_log_preserves_per_session_transport_without_inventing_remote_receipt(CallProtocol protocol, string name)
    {
        var line = Wire.Replace("\"protocol\":\"sip\"", $"\"protocol\":\"{name}\"") + "\n";
        Assert.True(NativeJsonEvent.TryParse(Encoding.UTF8.GetBytes(line), protocol, out var parsed, out var reason), reason);
        Assert.Equal("OpalVideoTransportChanged", parsed!.Event.GetType().Name);
        using var fields = JsonDocument.Parse(JsonSerializer.Serialize(parsed.Event, parsed.Event.GetType()));
        Assert.Equal(2, fields.RootElement.GetProperty("SessionId").GetInt32());
        Assert.Equal(102, fields.RootElement.GetProperty("PayloadType").GetInt32());
        Assert.Equal(240, fields.RootElement.GetProperty("PacketsSent").GetInt64());
        Assert.Equal(JsonValueKind.Null, fields.RootElement.GetProperty("RemotePacketsLost").ValueKind);
        var report = OpalSupportReport.Build(new { }, [parsed.Event], [], "", "");
        Assert.Contains("udp$10.136.51.11:49154", report);
        Assert.Contains("\"RemotePacketsLost\":null", report);
    }

    [Fact]
    public void Invalid_rtp_payload_cannot_be_reported_as_negotiated()
    {
        Assert.False(NativeJsonEvent.TryParse(Encoding.UTF8.GetBytes(Wire.Replace("\"payloadType\":102", "\"payloadType\":128") + "\n"), CallProtocol.Sip, out _, out _));
    }
}
