using TeleStudio.Contracts;
using TeleStudio.Desktop;
using System.Text;

namespace TeleStudio.Desktop.Tests;

public sealed class ProtocolConsoleEventParserTests
{
    [Fact]
    public void Windows_standard_stream_crlf_is_normalized_before_strict_json_parsing()
    {
        var windowsLine = Encoding.UTF8.GetBytes("{\"schema\":1,\"type\":\"media-ready\",\"sequence\":1,\"protocol\":\"h323\",\"sourceControl\":true,\"muteControl\":true,\"stats\":false,\"remoteSurface\":false,\"audioControl\":true,\"recording\":true,\"systemAudioControl\":true,\"roomMixerControl\":true}\r\n");

        var normalized = ProtocolConsoleProcess.NormalizeStandardStreamLine(windowsLine);

        Assert.Equal((byte)'\n', normalized.Span[^1]);
        Assert.NotEqual((byte)'\r', normalized.Span[^2]);
        Assert.True(NativeJsonEvent.TryParse(normalized.Span, CallProtocol.H323, out var parsed, out var reason), reason);
        Assert.Equal(new MediaCapabilitiesChanged(true, true, false, false, true, true, true, true), parsed!.Event);
    }

    public static IEnumerable<object[]> StrictRecords()
    {
        yield return ["{\"schema\":1,\"type\":\"media-ready\",\"sequence\":1,\"protocol\":\"sip\",\"sourceControl\":true,\"muteControl\":false,\"stats\":true,\"remoteSurface\":false,\"audioControl\":true,\"recording\":true}", new MediaCapabilitiesChanged(true, false, true, false, true, true)];
        yield return ["{\"schema\":1,\"type\":\"source-changed\",\"sequence\":2,\"protocol\":\"sip\",\"source\":\"desktop\"}", new LocalSourceChanged(VideoSource.Desktop)];
        yield return ["{\"schema\":1,\"type\":\"mute-changed\",\"sequence\":3,\"protocol\":\"sip\",\"muted\":true}", new MuteChanged(true)];
        yield return ["{\"schema\":1,\"type\":\"media-stats\",\"sequence\":4,\"protocol\":\"sip\",\"codec\":\"h265\",\"source\":\"camera\",\"width\":8192,\"height\":1,\"fps\":240,\"bitrateKbps\":100000,\"lossPercent\":100,\"jitterMs\":600000,\"oneWayLatencyMs\":0,\"droppedFrames\":9223372036854775807}", new MediaStatsChanged(CodecPreference.H265, VideoSource.Camera, 8192, 1, 240, 100000, 100, 600000, 0, long.MaxValue)];
        yield return ["{\"schema\":1,\"type\":\"media-error\",\"sequence\":5,\"protocol\":\"sip\",\"code\":\"capture-failed\",\"detail\":\"Safe detail\"}", new MediaError("capture-failed", "Safe detail")];
        yield return ["{\"schema\":1,\"type\":\"remote-surface\",\"sequence\":6,\"protocol\":\"sip\",\"hostHandle\":\"18446744073709551615\",\"width\":1920,\"height\":1080,\"format\":\"bgra8-unorm\",\"epoch\":\"1\"}", new RemoteSurfaceChanged(ulong.MaxValue, 1920, 1080, 1)];
        yield return ["{\"schema\":1,\"type\":\"audio-control-changed\",\"sequence\":7,\"protocol\":\"sip\",\"roomVolume\":100,\"micGain\":150,\"noiseIsolation\":true}", new AudioControlsChanged(100, 150, true)];
        yield return ["{\"schema\":1,\"type\":\"recording-state\",\"sequence\":8,\"protocol\":\"sip\",\"state\":\"completed\",\"micIncluded\":true,\"roomIncluded\":false,\"elapsedMs\":9223372036854775807,\"fileName\":\"lecture.mp4\"}", new RecordingStateChanged(RecordingState.Completed, true, false, long.MaxValue, "lecture.mp4")];
        yield return ["{\"schema\":1,\"type\":\"recording-error\",\"sequence\":9,\"protocol\":\"sip\",\"code\":\"write-failed\",\"detail\":\"Safe detail\",\"recoverablePart\":true,\"fileName\":\"lecture.part\"}", new RecordingError("write-failed", "Safe detail", true, "lecture.part")];
        yield return ["{\"schema\":1,\"type\":\"media-ready\",\"sequence\":10,\"protocol\":\"sip\",\"sourceControl\":true,\"muteControl\":true,\"stats\":false,\"remoteSurface\":false,\"audioControl\":true,\"recording\":true,\"systemAudioControl\":true,\"roomMixerControl\":true}", new MediaCapabilitiesChanged(true, true, false, false, true, true, true, true)];
        yield return ["{\"schema\":1,\"type\":\"microphone-mute-changed\",\"sequence\":11,\"protocol\":\"sip\",\"muted\":true}", new MicrophoneMuteChanged(true)];
        yield return ["{\"schema\":1,\"type\":\"system-audio-changed\",\"sequence\":12,\"protocol\":\"sip\",\"enabled\":true}", new SystemAudioChanged(true)];
        yield return ["{\"schema\":1,\"type\":\"room-audio-mute-changed\",\"sequence\":13,\"protocol\":\"sip\",\"muted\":true}", new RoomAudioMuteChanged(true)];
        yield return ["{\"schema\":1,\"type\":\"room-mixer-changed\",\"sequence\":14,\"protocol\":\"sip\",\"masterPercent\":300,\"bassDb\":-12,\"voiceDb\":0,\"trebleDb\":12}", new RoomMixerChanged(300, -12, 0, 12)];
        yield return ["{\"schema\":1,\"type\":\"room-audio-meter\",\"sequence\":15,\"protocol\":\"sip\",\"preLimiterPeakQ15\":32767,\"postLimiterPeakQ15\":29204,\"limiterGainReductionQ15\":1234,\"limiterActive\":true}", new RoomAudioMeterChanged(32767, 29204, 1234, true)];
        yield return ["{\"schema\":1,\"type\":\"audio-codec-state\",\"sequence\":16,\"protocol\":\"sip\",\"requested\":\"aac-ld\",\"negotiated\":\"g722\",\"sampleRate\":16000,\"channels\":1,\"bitrateKbps\":64,\"sendActive\":false,\"receiveActive\":true}", new AudioCodecStateChanged(AudioCodecKind.AacLd, AudioCodecKind.G722, 16000, 1, 64, false, true)];
    }

    [Theory]
    [InlineData("{\"schema\":1,\"type\":\"media-ready\",\"sequence\":1,\"protocol\":\"sip\",\"sourceControl\":true,\"muteControl\":true,\"stats\":false,\"remoteSurface\":false,\"audioControl\":true,\"recording\":true,\"roomMixerControl\":true,\"systemAudioControl\":true}\n")]
    [InlineData("{\"schema\":1,\"type\":\"media-ready\",\"sequence\":1,\"protocol\":\"sip\",\"sourceControl\":true,\"muteControl\":true,\"stats\":false,\"remoteSurface\":false,\"audioControl\":true,\"recording\":true,\"systemAudioControl\":true}\n")]
    [InlineData("{\"schema\":1,\"type\":\"room-mixer-changed\",\"sequence\":1,\"protocol\":\"sip\",\"masterPercent\":301,\"bassDb\":0,\"voiceDb\":0,\"trebleDb\":0}\n")]
    [InlineData("{\"schema\":1,\"type\":\"room-mixer-changed\",\"sequence\":1,\"protocol\":\"sip\",\"masterPercent\":100,\"bassDb\":-13,\"voiceDb\":0,\"trebleDb\":0}\n")]
    [InlineData("{\"schema\":1,\"type\":\"room-audio-meter\",\"sequence\":1,\"protocol\":\"sip\",\"preLimiterPeakQ15\":32768,\"postLimiterPeakQ15\":0,\"limiterGainReductionQ15\":0,\"limiterActive\":false}\n")]
    [InlineData("{\"schema\":1,\"type\":\"audio-codec-state\",\"sequence\":1,\"protocol\":\"sip\",\"requested\":\"aac-ld\",\"negotiated\":\"g711\",\"sampleRate\":8000,\"channels\":1,\"bitrateKbps\":64,\"sendActive\":true,\"receiveActive\":false}\n")]
    [InlineData("{\"schema\":1,\"type\":\"audio-codec-state\",\"sequence\":1,\"protocol\":\"sip\",\"requested\":\"aac-ld\",\"negotiated\":\"aac-ld\",\"sampleRate\":48000,\"channels\":2,\"bitrateKbps\":128,\"sendActive\":false,\"receiveActive\":false}\n")]
    public void Strict_jsonl_parser_rejects_partial_swapped_and_out_of_range_audio_records(string record)
    {
        Assert.False(NativeJsonEvent.TryParse(Encoding.UTF8.GetBytes(record), CallProtocol.Sip, out _, out _));
    }

    [Theory]
    [MemberData(nameof(StrictRecords))]
    public void Strict_jsonl_parser_accepts_all_nine_exact_records(string json, EngineEvent expected)
    {
        Assert.True(NativeJsonEvent.TryParse(Encoding.UTF8.GetBytes(json + "\n"), CallProtocol.Sip, out var parsed, out var reason), reason);
        Assert.Equal(expected, parsed!.Event);
    }

    [Theory]
    [InlineData("{\"type\":\"mute-changed\",\"schema\":1,\"sequence\":1,\"protocol\":\"sip\",\"muted\":true}\n")]
    [InlineData("{\"schema\":1,\"type\":\"mute-changed\",\"sequence\":1,\"sequence\":2,\"protocol\":\"sip\",\"muted\":true}\n")]
    [InlineData("{\"schema\":1,\"type\":\"mute-changed\",\"sequence\":1,\"protocol\":\"sip\",\"muted\":true,\"extra\":false}\n")]
    [InlineData("{\"schema\":2,\"type\":\"mute-changed\",\"sequence\":1,\"protocol\":\"sip\",\"muted\":true}\n")]
    [InlineData("{\"schema\":1,\"type\":\"mute-changed\",\"sequence\":1,\"protocol\":\"h323\",\"muted\":true}\n")]
    [InlineData("{\"schema\":1, \"type\":\"mute-changed\",\"sequence\":1,\"protocol\":\"sip\",\"muted\":true}\n")]
    [InlineData("{\"schema\":1,\"type\":\"mute-changed\",\"sequence\":1,\"protocol\":\"sip\",\"muted\":true}")]
    public void Strict_jsonl_parser_rejects_schema_order_duplicates_unknown_emitter_whitespace_and_framing(string record)
    {
        Assert.False(NativeJsonEvent.TryParse(Encoding.UTF8.GetBytes(record), CallProtocol.Sip, out var parsed, out _));
        Assert.Null(parsed);
    }

    [Fact]
    public void Strict_jsonl_parser_rejects_invalid_utf8_bom_and_unpaired_escaped_surrogate()
    {
        var invalidUtf8 = Encoding.UTF8.GetBytes("{\"schema\":1,\"type\":\"media-error\",\"sequence\":1,\"protocol\":\"sip\",\"code\":\"x\",\"detail\":\"x\"}\n");
        invalidUtf8[^4] = 0xff;
        Assert.False(NativeJsonEvent.TryParse(invalidUtf8, CallProtocol.Sip, out _, out _));

        var bom = Encoding.UTF8.GetPreamble().Concat(Encoding.UTF8.GetBytes("{\"schema\":1}\n")).ToArray();
        Assert.False(NativeJsonEvent.TryParse(bom, CallProtocol.Sip, out _, out _));

        var surrogate = Encoding.UTF8.GetBytes("{\"schema\":1,\"type\":\"media-error\",\"sequence\":1,\"protocol\":\"sip\",\"code\":\"x\",\"detail\":\"\\uD800\"}\n");
        Assert.False(NativeJsonEvent.TryParse(surrogate, CallProtocol.Sip, out _, out _));
    }

    [Theory]
    [InlineData("C:\\\\Users\\\\essa\\\\lecture.mp4")]
    [InlineData("../lecture.mp4")]
    [InlineData("CON.mp4")]
    [InlineData("lecture. ")]
    [InlineData("lecture.part")]
    public void Completed_recording_rejects_unsafe_or_cross_field_filename(string fileName)
    {
        var escaped = fileName.Replace("\\", "\\\\");
        var json = $"{{\"schema\":1,\"type\":\"recording-state\",\"sequence\":1,\"protocol\":\"sip\",\"state\":\"completed\",\"micIncluded\":true,\"roomIncluded\":false,\"elapsedMs\":1,\"fileName\":\"{escaped}\"}}\n";
        Assert.False(NativeJsonEvent.TryParse(Encoding.UTF8.GetBytes(json), CallProtocol.Sip, out _, out _));
    }

    [Fact]
    public void Legacy_source_confirmation_is_not_accepted()
    {
        Assert.False(ProtocolConsoleEventParser.TryParse(
            CallProtocol.Sip, "source-changed source=desktop", "10.0.0.1", out _));
    }

    [Fact]
    public void H323_incoming_line_preserves_remote_address()
    {
        var parsed = ProtocolConsoleEventParser.TryParse(
            CallProtocol.H323,
            "incoming-call remote=\"10.136.51.25\" action=type-answer-or-reject",
            fallbackRemoteAddress: null,
            out var engineEvent);

        Assert.True(parsed);
        Assert.Equal(new IncomingCall("10.136.51.25", CallProtocol.H323), engineEvent);
    }

    [Fact]
    public void H323_dial_progress_keeps_the_user_facing_destination()
    {
        var parsed = ProtocolConsoleEventParser.TryParse(
            CallProtocol.H323,
            "dial-start destination=\"ip$10.136.51.25:1720\" gatekeeper=disabled",
            fallbackRemoteAddress: "10.136.51.25",
            out var engineEvent);

        Assert.True(parsed);
        Assert.Equal(new CallProgress("10.136.51.25", CallProtocol.H323, CallState.Calling), engineEvent);
    }

    [Fact]
    public void Sip_confirmed_line_uses_the_active_destination_when_console_omits_it()
    {
        var parsed = ProtocolConsoleEventParser.TryParse(
            CallProtocol.Sip,
            "call 0: CONFIRMED (OK)",
            fallbackRemoteAddress: "10.136.51.26",
            out var engineEvent);

        Assert.True(parsed);
        Assert.Equal(new CallConnected("10.136.51.26", CallProtocol.Sip, 0, 0, 0), engineEvent);
    }

    [Fact]
    public void Protocol_error_is_exposed_as_a_failed_call_with_detail()
    {
        var parsed = ProtocolConsoleEventParser.TryParse(
            CallProtocol.Sip,
            "startup error: Connection refused",
            fallbackRemoteAddress: "10.136.51.26",
            out var engineEvent);

        Assert.True(parsed);
        Assert.Equal(new CallFailed("startup-error", "Connection refused"), engineEvent);
    }

    [Theory]
    [InlineData("CALLING", CallState.Calling)]
    [InlineData("EARLY", CallState.Negotiating)]
    [InlineData("CONNECTING", CallState.Negotiating)]
    public void Sip_progress_states_preserve_the_correlated_remote(string state, CallState expected)
    {
        var parsed = ProtocolConsoleEventParser.TryParse(
            CallProtocol.Sip,
            $"call 4: {state} (Ringing)",
            fallbackRemoteAddress: "10.0.0.23",
            out var engineEvent);

        Assert.True(parsed);
        Assert.Equal(new CallProgress("10.0.0.23", CallProtocol.Sip, expected), engineEvent);
    }

    [Fact]
    public void Sip_command_error_without_command_correlation_is_diagnostic_only()
    {
        var parsed = ProtocolConsoleEventParser.TryParse(
            CallProtocol.Sip,
            "command error: no call is active",
            fallbackRemoteAddress: "10.0.0.23",
            out var engineEvent);

        Assert.False(parsed);
        Assert.Null(engineEvent);
    }

    [Fact]
    public void Unrecognized_line_does_not_emit_a_state_transition()
    {
        var parsed = ProtocolConsoleEventParser.TryParse(
            CallProtocol.H323,
            "capabilities: G.711 G.722",
            fallbackRemoteAddress: null,
            out var engineEvent);

        Assert.False(parsed);
        Assert.Null(engineEvent);
    }
}
