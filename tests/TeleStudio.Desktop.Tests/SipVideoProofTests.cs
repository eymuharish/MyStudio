using System.Text;
using TeleStudio.Contracts;
using TeleStudio.Desktop;
namespace TeleStudio.Desktop.Tests;
public sealed class SipVideoProofTests
{
    [Theory]
    [InlineData("\"type\":\"sip-call-mode\",\"sequence\":1,\"protocol\":\"sip\",\"mode\":\"video-only\"")]
    [InlineData("\"type\":\"sip-audio-unavailable\",\"sequence\":1,\"protocol\":\"sip\",\"reason\":\"video-only-requested\",\"detail\":\"Audio disabled for diagnostic call\"")]
    [InlineData("\"type\":\"sip-outbound-video-proof\",\"sequence\":1,\"protocol\":\"sip\",\"sourceFrames\":32,\"encodedFrameEnds\":28,\"encoderFps\":0,\"rtpPacketsSent\":900,\"rtpBytesSent\":120000,\"peerReportAvailable\":true,\"peerExtendedSequence\":905,\"peerLossPercent\":0.5,\"peerCumulativeLoss\":-1,\"peerJitterMs\":1.5")]
    public void Actual_video_mode_and_RTCP_evidence_pass_the_managed_wire_boundary(string fields)
    {
        Assert.True(NativeJsonEvent.TryParse(Encoding.UTF8.GetBytes("{\"schema\":1," + fields + "}\n"), CallProtocol.Sip, out _, out var reason), reason);
    }
    [Theory]
    [InlineData("true", 42, 42, true)]
    [InlineData("true", 42, 43, false)]
    [InlineData("false", 42, 42, false)]
    public void Peer_report_requires_matching_sender_and_truthful_report_presence(string report, uint sender, uint reported, bool accepted)
    {
        var line = "{\"schema\":1,\"type\":\"sip-outbound-video-proof\",\"sequence\":1,\"protocol\":\"sip\",\"sourceFrames\":32,\"encodedFrameEnds\":28,\"encoderFps\":0,\"rtpPacketsSent\":900,\"rtpBytesSent\":120000,\"peerReportAvailable\":" + report + ",\"peerExtendedSequence\":905,\"peerLossPercent\":0.5,\"peerCumulativeLoss\":-1,\"peerJitterMs\":1.5,\"senderSsrc\":" + sender + ",\"peerReportedSsrc\":" + reported + "}\n";
        Assert.Equal(accepted, NativeJsonEvent.TryParse(Encoding.UTF8.GetBytes(line), CallProtocol.Sip, out var parsed, out _));
        if (accepted) Assert.Null(Assert.IsType<SipOutboundVideoProofChanged>(parsed!.Event).EncoderFps);
    }
}
