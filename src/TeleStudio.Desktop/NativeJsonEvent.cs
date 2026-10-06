using System.Buffers;
using System.Globalization;
using System.IO;
using System.Text;
using System.Text.Json;
using TeleStudio.Contracts;

namespace TeleStudio.Desktop;

public sealed record NativeJsonEvent(long Sequence, CallProtocol Protocol, EngineEvent Event)
{
    private static readonly UTF8Encoding StrictUtf8 = new(false, true);

    public static bool TryParse(
        ReadOnlySpan<byte> line,
        CallProtocol emitterProtocol,
        out NativeJsonEvent? value,
        out string rejectionReason)
    {
        value = null;
        rejectionReason = "invalid-record";
        try
        {
            if (line.Length < 3 || line[^1] != (byte)'\n' || line[..^1].Contains((byte)'\n') || line[0] == 0xef)
            {
                return false;
            }
            var json = line[..^1];
            _ = StrictUtf8.GetString(json);
            if (!IsCompact(json))
            {
                rejectionReason = "non-compact-record";
                return false;
            }

            var reader = new Utf8JsonReader(json, new JsonReaderOptions
            {
                AllowTrailingCommas = false,
                CommentHandling = JsonCommentHandling.Disallow,
                MaxDepth = 4,
            });
            Require(ref reader, JsonTokenType.StartObject);
            RequireProperty(ref reader, "schema");
            Require(ref reader, JsonTokenType.Number);
            if (!reader.ValueSpan.SequenceEqual("1"u8))
            {
                throw new FormatException("schema");
            }
            RequireProperty(ref reader, "type");
            var type = ReadString(ref reader);
            RequireProperty(ref reader, "sequence");
            var sequence = ReadInt64(ref reader, 1, long.MaxValue);
            RequireProperty(ref reader, "protocol");
            var protocolToken = ReadString(ref reader);
            var protocol = protocolToken switch
            {
                "sip" => CallProtocol.Sip,
                "h323" => CallProtocol.H323,
                _ => throw new FormatException("protocol")
            };
            if (protocol != emitterProtocol)
            {
                rejectionReason = "protocol-mismatch";
                return false;
            }

            var engineEvent = type switch
            {
                "aac-availability" => ReadAacAvailability(ref reader),
                "media-ready" => ReadMediaReady(ref reader),
                "call-hold-changed" => ReadCallHold(ref reader),
                "content-sharing-state" => ReadContentSharing(ref reader),
                "sip-connection-health" when protocol == CallProtocol.Sip => ReadSipHealth(ref reader),
                "opal-connection-health" => ReadSipHealth(ref reader),
                "opal-video-transport" => ReadOpalVideoTransport(ref reader),
                "sip-registration-state" when protocol == CallProtocol.Sip => ReadSipRegistration(ref reader),
                "sip-security-state" when protocol == CallProtocol.Sip => ReadSipSecurity(ref reader),
                "sip-session-timer" when protocol == CallProtocol.Sip => ReadSipSessionTimer(ref reader),
                "sip-call-mode" when protocol == CallProtocol.Sip => ReadSipCallMode(ref reader),
                "sip-audio-unavailable" when protocol == CallProtocol.Sip => ReadSipAudioUnavailable(ref reader),
                "sip-outbound-video-proof" when protocol == CallProtocol.Sip => ReadSipVideoProof(ref reader),
                "source-changed" => ReadSourceChanged(ref reader),
                "mute-changed" => ReadMuteChanged(ref reader),
                "microphone-mute-changed" => ReadMicrophoneMuteChanged(ref reader),
                "system-audio-changed" => ReadSystemAudioChanged(ref reader),
                "room-audio-mute-changed" => ReadRoomAudioMuteChanged(ref reader),
                "room-mixer-changed" => ReadRoomMixerChanged(ref reader),
                "room-audio-meter" => ReadRoomAudioMeter(ref reader),
                "media-stats" => ReadMediaStats(ref reader),
                "media-error" => ReadMediaError(ref reader),
                "remote-surface" => ReadRemoteSurface(ref reader),
                "local-surface" => ReadLocalSurface(ref reader),
                "video-state" => ReadVideoState(ref reader),
                "audio-control-changed" => ReadAudioControls(ref reader),
                "echo-cancellation-state" => ReadEchoCancellation(ref reader),
                "audio-codec-state" => ReadAudioCodecState(ref reader),
                "audio-profile-applied" => ReadAudioProfileApplied(ref reader),
                "audio-direction-state" => ReadAudioDirection(ref reader),
                "network-video-applied" => ReadNetworkVideoApplied(ref reader),
                "transport-stats" => ReadTransportStatistics(ref reader),
                "recording-state" => ReadRecordingState(ref reader),
                "recording-error" => ReadRecordingError(ref reader),
                _ => throw new FormatException("type")
            };
            Require(ref reader, JsonTokenType.EndObject);
            if (reader.Read())
            {
                throw new FormatException("trailing-token");
            }
            value = new NativeJsonEvent(sequence, protocol, engineEvent);
            rejectionReason = string.Empty;
            return true;
        }
        catch (Exception error) when (error is JsonException or FormatException or DecoderFallbackException or OverflowException or InvalidOperationException)
        {
            rejectionReason = MapReason(error);
            return false;
        }
    }

    public static bool IsSafeRecordingFileName(string fileName)
    {
        if (fileName.Length == 0 || !IsValidScalarString(fileName, 255))
        {
            return false;
        }
        if (fileName is "." or ".." || fileName.EndsWith(' ') || fileName.EndsWith('.'))
        {
            return false;
        }
        foreach (var character in fileName)
        {
            if (character is < ' ' or '\u007f' or '/' or '\\' or ':' or '*' or '?' or '"' or '<' or '>' or '|')
            {
                return false;
            }
        }
        if (Path.IsPathRooted(fileName) || !string.Equals(Path.GetFileName(fileName), fileName, StringComparison.Ordinal))
        {
            return false;
        }
        var stem = fileName.Split('.')[0];
        if (stem.Equals("CON", StringComparison.OrdinalIgnoreCase) ||
            stem.Equals("PRN", StringComparison.OrdinalIgnoreCase) ||
            stem.Equals("AUX", StringComparison.OrdinalIgnoreCase) ||
            stem.Equals("NUL", StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }
        return !(stem.Length == 4 &&
            (stem.StartsWith("COM", StringComparison.OrdinalIgnoreCase) || stem.StartsWith("LPT", StringComparison.OrdinalIgnoreCase)) &&
            stem[3] is >= '1' and <= '9');
    }

    private static EngineEvent ReadAacAvailability(ref Utf8JsonReader reader)
    {
        RequireProperty(ref reader, "available");
        var available = ReadBoolean(ref reader);
        RequireProperty(ref reader, "detail");
        return new AacAvailabilityChanged(available, ReadBoundedString(ref reader, 2048));
    }

    private static EngineEvent ReadMediaReady(ref Utf8JsonReader reader)
    {
        RequireProperty(ref reader, "sourceControl");
        var source = ReadBoolean(ref reader);
        RequireProperty(ref reader, "muteControl");
        var mute = ReadBoolean(ref reader);
        RequireProperty(ref reader, "stats");
        var stats = ReadBoolean(ref reader);
        RequireProperty(ref reader, "remoteSurface");
        var surface = ReadBoolean(ref reader);
        RequireProperty(ref reader, "audioControl");
        var audio = ReadBoolean(ref reader);
        RequireProperty(ref reader, "recording");
        var recording = ReadBoolean(ref reader);
        var lookahead = reader;
        if (lookahead.Read() && lookahead.TokenType == JsonTokenType.EndObject)
        {
            return new MediaCapabilitiesChanged(source, mute, stats, surface, audio, recording);
        }
        RequireProperty(ref reader, "systemAudioControl");
        var systemAudio = ReadBoolean(ref reader);
        RequireProperty(ref reader, "roomMixerControl");
        var roomMixer = ReadBoolean(ref reader);
        lookahead = reader;
        if (lookahead.Read() && lookahead.TokenType == JsonTokenType.EndObject)
            return new MediaCapabilitiesChanged(source, mute, stats, surface, audio, recording, systemAudio, roomMixer);
        RequireProperty(ref reader, "videoControl");
        var video = ReadBoolean(ref reader);
        lookahead = reader;
        if (lookahead.Read() && lookahead.TokenType == JsonTokenType.EndObject)
            return new MediaCapabilitiesChanged(source, mute, stats, surface, audio, recording, systemAudio, roomMixer, video);
        RequireProperty(ref reader, "holdControl");
        var hold = ReadBoolean(ref reader);
        RequireProperty(ref reader, "dtmfControl");
        return new MediaCapabilitiesChanged(source, mute, stats, surface, audio, recording, systemAudio, roomMixer, video, hold, ReadBoolean(ref reader));
    }

    private static EngineEvent ReadCallHold(ref Utf8JsonReader reader)
    {
        RequireProperty(ref reader, "held");
        return new CallHoldChanged(ReadBoolean(ref reader));
    }

    private static EngineEvent ReadContentSharing(ref Utf8JsonReader reader)
    {
        RequireProperty(ref reader, "mode");
        var mode = ReadString(ref reader);
        if (mode is not ("h239" or "bfcp" or "main-video" or "none")) throw new FormatException("content-mode");
        RequireProperty(ref reader, "active");
        var active = ReadBoolean(ref reader);
        RequireProperty(ref reader, "detail");
        return new ContentSharingStateChanged(mode, active, ReadString(ref reader));
    }

    private static EngineEvent ReadSipRegistration(ref Utf8JsonReader reader)
    {
        RequireProperty(ref reader, "state");
        var state = ReadString(ref reader);
        if (state is not ("none" or "progress" or "ok" or "failed" or "cleared")) throw new FormatException("registration-state");
        RequireProperty(ref reader, "detail");
        return new SipRegistrationStateChanged(state, ReadBoundedString(ref reader, 2048));
    }
    private static EngineEvent ReadSipSecurity(ref Utf8JsonReader reader)
    {
        RequireProperty(ref reader, "transport");
        var transport = ReadString(ref reader);
        if (transport is not ("udp" or "tcp" or "tls" or "unknown")) throw new FormatException("sip-transport");
        RequireProperty(ref reader, "mediaEncryption");
        var encryption = ReadString(ref reader);
        if (encryption is not ("none" or "srtp" or "unknown")) throw new FormatException("sip-encryption");
        return new SipSecurityStateChanged(transport, encryption);
    }
    private static EngineEvent ReadSipHealth(ref Utf8JsonReader reader)
    {
        var atx = ReadNullableMeasurement(ref reader, "audioTxKbps", 1000000);
        var arx = ReadNullableMeasurement(ref reader, "audioRxKbps", 1000000);
        var vtx = ReadNullableMeasurement(ref reader, "videoTxKbps", 1000000);
        var vrx = ReadNullableMeasurement(ref reader, "videoRxKbps", 1000000);
        var loss = ReadNullableMeasurement(ref reader, "lossPercent", 100);
        var jitter = ReadNullableMeasurement(ref reader, "jitterBufferMs", 600000);
        var rtt = ReadNullableMeasurement(ref reader, "roundTripMs", 600000);
        RequireProperty(ref reader, "capturedSamples"); var captured = ReadInt64(ref reader, 0, long.MaxValue);
        RequireProperty(ref reader, "decodedAudioSamples"); var decoded = ReadInt64(ref reader, 0, long.MaxValue);
        RequireProperty(ref reader, "sentFrames"); var sent = ReadInt64(ref reader, 0, long.MaxValue);
        RequireProperty(ref reader, "receivedFrames"); var received = ReadInt64(ref reader, 0, long.MaxValue);
        RequireProperty(ref reader, "decodedNonzeroSamples"); var nonzero = ReadInt64(ref reader, 0, decoded);
        RequireProperty(ref reader, "deviceSubmittedNonzeroSamples"); var submitted = ReadInt64(ref reader, 0, long.MaxValue);
        return new SipConnectionHealthChanged(atx, arx, vtx, vrx, loss, jitter, rtt, captured, decoded, sent, received, nonzero, submitted);
    }
    private static EngineEvent ReadOpalVideoTransport(ref Utf8JsonReader reader)
    {
        RequireProperty(ref reader, "sessionId"); var session = (int)ReadInt64(ref reader, 1, 65535);
        RequireProperty(ref reader, "direction"); var direction = ReadString(ref reader);
        if (direction is not ("send" or "receive")) throw new FormatException("video-direction");
        RequireProperty(ref reader, "role"); var role = ReadString(ref reader);
        if (role is not ("main" or "presentation" or "unknown")) throw new FormatException("video-role");
        RequireProperty(ref reader, "codec"); var codec = ReadBoundedString(ref reader, 256);
        RequireProperty(ref reader, "payloadType"); var payload = (int)ReadInt64(ref reader, 0, 127);
        RequireProperty(ref reader, "width"); var width = (int)ReadInt64(ref reader, 0, 8192);
        RequireProperty(ref reader, "height"); var height = (int)ReadInt64(ref reader, 0, 8192);
        var packetization = (int?)ReadNullableCounter(ref reader, "packetizationMode", 0, 2);
        RequireProperty(ref reader, "profileLevelId"); var profile = ReadBoundedString(ref reader, 64);
        RequireProperty(ref reader, "localRtp"); var local = ReadBoundedString(ref reader, 256);
        RequireProperty(ref reader, "remoteRtp"); var remote = ReadBoundedString(ref reader, 256);
        RequireProperty(ref reader, "packetsSent"); var sent = ReadInt64(ref reader, 0, long.MaxValue);
        RequireProperty(ref reader, "octetsSent"); var bytes = ReadInt64(ref reader, 0, long.MaxValue);
        RequireProperty(ref reader, "markersSent"); var markers = ReadInt64(ref reader, 0, long.MaxValue);
        RequireProperty(ref reader, "packetsReceived"); var received = ReadInt64(ref reader, 0, long.MaxValue);
        RequireProperty(ref reader, "octetsReceived"); var receivedBytes = ReadInt64(ref reader, 0, long.MaxValue);
        var lost = ReadNullableCounter(ref reader, "remotePacketsLost", -8388608, 8388607);
        RequireProperty(ref reader, "senderSsrc"); var ssrc = (uint)ReadInt64(ref reader, 0, uint.MaxValue);
        return new OpalVideoTransportChanged(session, direction, role, codec, payload, width, height,
            packetization, profile, local, remote, sent, bytes, markers, received, receivedBytes, lost, ssrc);
    }
    private static EngineEvent ReadSipCallMode(ref Utf8JsonReader reader)
    {
        RequireProperty(ref reader, "mode"); var mode = ReadString(ref reader);
        if (mode is not ("audio-video" or "video-only")) throw new FormatException("sip-call-mode");
        return new SipCallModeChanged(mode);
    }
    private static EngineEvent ReadSipAudioUnavailable(ref Utf8JsonReader reader)
    { RequireProperty(ref reader, "reason"); var reason = ReadBoundedString(ref reader, 128); RequireProperty(ref reader, "detail"); return new SipAudioUnavailable(reason, ReadBoundedString(ref reader, 2048)); }
    private static long? ReadNullableCounter(ref Utf8JsonReader reader, string name, long minimum = 0, long maximum = long.MaxValue)
    {
        RequireProperty(ref reader, name); var lookahead = reader;
        if (lookahead.Read() && lookahead.TokenType == JsonTokenType.Null) { reader = lookahead; return null; }
        return ReadInt64(ref reader, minimum, maximum);
    }
    private static EngineEvent ReadSipVideoProof(ref Utf8JsonReader reader)
    {
        RequireProperty(ref reader, "sourceFrames"); var source = ReadInt64(ref reader, 0, long.MaxValue);
        RequireProperty(ref reader, "encodedFrameEnds"); var encoded = ReadInt64(ref reader, 0, long.MaxValue);
        var fps = ReadNullableMeasurement(ref reader, "encoderFps", 1000); if (fps == 0) fps = null;
        RequireProperty(ref reader, "rtpPacketsSent"); var packets = ReadInt64(ref reader, 0, long.MaxValue);
        RequireProperty(ref reader, "rtpBytesSent"); var bytes = ReadInt64(ref reader, 0, long.MaxValue);
        RequireProperty(ref reader, "peerReportAvailable"); var report = ReadBoolean(ref reader);
        var sequence = ReadNullableCounter(ref reader, "peerExtendedSequence");
        var loss = ReadNullableMeasurement(ref reader, "peerLossPercent", 100);
        var cumulative = ReadNullableCounter(ref reader, "peerCumulativeLoss", -8388608, 8388607);
        var jitter = ReadNullableMeasurement(ref reader, "peerJitterMs", 600000);
        uint? sender = null, reported = null; var lookahead = reader;
        if (lookahead.Read() && lookahead.TokenType == JsonTokenType.PropertyName && lookahead.ValueTextEquals("senderSsrc"))
        {
            sender = (uint?)ReadNullableCounter(ref reader, "senderSsrc", 0, uint.MaxValue);
            reported = (uint?)ReadNullableCounter(ref reader, "peerReportedSsrc", 0, uint.MaxValue);
        }
        if (!report && (sequence is not null || loss is not null || cumulative is not null || jitter is not null || reported is not null) || sender is not null && reported is not null && sender != reported)
            throw new FormatException("sip-video-proof-report");
        return new SipOutboundVideoProofChanged(source, encoded, fps, packets, bytes, report, sequence, loss, cumulative, jitter, sender, reported);
    }

    private static EngineEvent ReadSourceChanged(ref Utf8JsonReader reader)
    {
        RequireProperty(ref reader, "source");
        return new LocalSourceChanged(ReadSource(ref reader));
    }

    private static EngineEvent ReadMuteChanged(ref Utf8JsonReader reader)
    {
        RequireProperty(ref reader, "muted");
        return new MuteChanged(ReadBoolean(ref reader));
    }

    private static EngineEvent ReadMicrophoneMuteChanged(ref Utf8JsonReader reader)
    {
        RequireProperty(ref reader, "muted");
        return new MicrophoneMuteChanged(ReadBoolean(ref reader));
    }

    private static EngineEvent ReadSystemAudioChanged(ref Utf8JsonReader reader)
    {
        RequireProperty(ref reader, "enabled");
        return new SystemAudioChanged(ReadBoolean(ref reader));
    }

    private static EngineEvent ReadSipSessionTimer(ref Utf8JsonReader reader)
    {
        RequireProperty(ref reader, "sessionIntervalSeconds");
        var interval = checked((int)ReadInt64(ref reader, 0, int.MaxValue));
        RequireProperty(ref reader, "refreshAfterSeconds");
        var refresh = checked((int)ReadInt64(ref reader, 0, int.MaxValue));
        RequireProperty(ref reader, "localRefresher");
        var local = ReadBoolean(ref reader);
        if ((interval != 0 && interval < 90) || (local ? interval == 0 || refresh != interval / 2 : refresh != 0))
            throw new FormatException("session-timer");
        return new SipSessionTimerChanged(interval, refresh, local);
    }
    private static EngineEvent ReadEchoCancellation(ref Utf8JsonReader reader)
    {
        RequireProperty(ref reader, "available"); var available = ReadBoolean(ref reader);
        RequireProperty(ref reader, "enabled"); var enabled = ReadBoolean(ref reader);
        if (enabled && !available) throw new FormatException("echo-unavailable");
        return new EchoCancellationChanged(available, enabled);
    }

    private static EngineEvent ReadRoomAudioMuteChanged(ref Utf8JsonReader reader)
    {
        RequireProperty(ref reader, "muted");
        return new RoomAudioMuteChanged(ReadBoolean(ref reader));
    }

    private static EngineEvent ReadRoomMixerChanged(ref Utf8JsonReader reader)
    {
        RequireProperty(ref reader, "masterPercent");
        var master = checked((int)ReadInt64(ref reader, 0, 300));
        RequireProperty(ref reader, "bassDb");
        var bass = checked((int)ReadInt64(ref reader, -12, 12));
        RequireProperty(ref reader, "voiceDb");
        var voice = checked((int)ReadInt64(ref reader, -12, 12));
        RequireProperty(ref reader, "trebleDb");
        var treble = checked((int)ReadInt64(ref reader, -12, 12));
        var peek = reader;
        var noise = 0;
        if (peek.Read() && peek.TokenType == JsonTokenType.PropertyName && peek.ValueTextEquals("noiseReductionDb"))
        {
            RequireProperty(ref reader, "noiseReductionDb");
            noise = checked((int)ReadInt64(ref reader, 0, 30));
        }
        return new RoomMixerChanged(master, bass, voice, treble, noise);
    }

    private static EngineEvent ReadRoomAudioMeter(ref Utf8JsonReader reader)
    {
        RequireProperty(ref reader, "preLimiterPeakQ15");
        var pre = checked((int)ReadInt64(ref reader, 0, 32767));
        RequireProperty(ref reader, "postLimiterPeakQ15");
        var post = checked((int)ReadInt64(ref reader, 0, 32767));
        RequireProperty(ref reader, "limiterGainReductionQ15");
        var reduction = checked((int)ReadInt64(ref reader, 0, 32767));
        RequireProperty(ref reader, "limiterActive");
        return new RoomAudioMeterChanged(pre, post, reduction, ReadBoolean(ref reader));
    }

    private static EngineEvent ReadMediaStats(ref Utf8JsonReader reader)
    {
        RequireProperty(ref reader, "codec");
        var codec = ReadString(ref reader) switch
        {
            "h264" => CodecPreference.H264,
            "h265" => CodecPreference.H265,
            _ => throw new FormatException("codec")
        };
        RequireProperty(ref reader, "source");
        var source = ReadSource(ref reader);
        RequireProperty(ref reader, "width");
        var width = checked((int)ReadInt64(ref reader, 1, 8192));
        RequireProperty(ref reader, "height");
        var height = checked((int)ReadInt64(ref reader, 1, 8192));
        RequireProperty(ref reader, "fps");
        var fps = checked((int)ReadInt64(ref reader, 1, 240));
        RequireProperty(ref reader, "bitrateKbps");
        var bitrate = checked((int)ReadInt64(ref reader, 0, 100000));
        RequireProperty(ref reader, "lossPercent");
        var loss = ReadDouble(ref reader, 0, 100);
        RequireProperty(ref reader, "jitterMs");
        var jitter = ReadDouble(ref reader, 0, 600000);
        RequireProperty(ref reader, "oneWayLatencyMs");
        var latency = ReadDouble(ref reader, 0, 600000);
        RequireProperty(ref reader, "droppedFrames");
        var dropped = ReadInt64(ref reader, 0, long.MaxValue);
        return new MediaStatsChanged(codec, source, width, height, fps, bitrate, loss, jitter, latency, dropped);
    }

    private static EngineEvent ReadMediaError(ref Utf8JsonReader reader)
    {
        RequireProperty(ref reader, "code");
        var code = ReadErrorCode(ref reader);
        RequireProperty(ref reader, "detail");
        return new MediaError(code, ReadBoundedString(ref reader, 2048));
    }

    private static EngineEvent ReadRemoteSurface(ref Utf8JsonReader reader)
    {
        RequireProperty(ref reader, "hostHandle");
        var handle = ReadCanonicalUInt64(ref reader);
        RequireProperty(ref reader, "width");
        var width = checked((int)ReadInt64(ref reader, 1, 8192));
        RequireProperty(ref reader, "height");
        var height = checked((int)ReadInt64(ref reader, 1, 8192));
        RequireProperty(ref reader, "format");
        if (ReadString(ref reader) != "bgra8-unorm")
        {
            throw new FormatException("format");
        }
        RequireProperty(ref reader, "epoch");
        return new RemoteSurfaceChanged(handle, width, height, ReadCanonicalUInt64(ref reader));
    }

    private static EngineEvent ReadLocalSurface(ref Utf8JsonReader reader)
    {
        RequireProperty(ref reader, "hostHandle");
        var handle = ReadCanonicalUInt64(ref reader);
        RequireProperty(ref reader, "width");
        var width = checked((int)ReadInt64(ref reader, 1, 8192));
        RequireProperty(ref reader, "height");
        var height = checked((int)ReadInt64(ref reader, 1, 8192));
        RequireProperty(ref reader, "format");
        if (ReadString(ref reader) != "bgra8-unorm") throw new FormatException("format");
        RequireProperty(ref reader, "epoch");
        return new LocalSurfaceChanged(handle, width, height, ReadCanonicalUInt64(ref reader));
    }

    private static EngineEvent ReadVideoState(ref Utf8JsonReader reader)
    {
        RequireProperty(ref reader, "cameraRequested"); var cameraRequested = ReadBoolean(ref reader);
        RequireProperty(ref reader, "desktopRequested"); var desktopRequested = ReadBoolean(ref reader);
        RequireProperty(ref reader, "cameraActive"); var cameraActive = ReadBoolean(ref reader);
        RequireProperty(ref reader, "desktopActive"); var desktopActive = ReadBoolean(ref reader);
        RequireProperty(ref reader, "resolution");
        var resolution = ReadString(ref reader) switch { "auto" => VideoResolution.Auto, "sd" => VideoResolution.Sd, "hd" => VideoResolution.Hd, "fhd" => VideoResolution.Fhd, _ => throw new FormatException("resolution") };
        RequireProperty(ref reader, "bitrateKbps");
        int? bitrate = ReadNullableInt(ref reader, 128, 20000);
        RequireProperty(ref reader, "appliedWidth"); var width = checked((int)ReadInt64(ref reader, 0, 8192));
        RequireProperty(ref reader, "appliedHeight"); var height = checked((int)ReadInt64(ref reader, 0, 8192));
        if ((width == 0) != (height == 0) || (width != 0 && (width % 2 != 0 || height % 2 != 0))) throw new FormatException("applied-dimensions");
        RequireProperty(ref reader, "appliedBitrateKbps"); var appliedBitrate = checked((int)ReadInt64(ref reader, 0, 20000));
        RequireProperty(ref reader, "overlaySize"); var size = ReadString(ref reader) switch { "small" => CameraOverlaySize.Small, "medium" => CameraOverlaySize.Medium, "large" => CameraOverlaySize.Large, _ => throw new FormatException("overlay-size") };
        RequireProperty(ref reader, "overlayPosition"); var position = ReadString(ref reader) switch { "top-left" => CameraOverlayPosition.TopLeft, "top-right" => CameraOverlayPosition.TopRight, "bottom-left" => CameraOverlayPosition.BottomLeft, "bottom-right" => CameraOverlayPosition.BottomRight, _ => throw new FormatException("overlay-position") };
        RequireProperty(ref reader, "revision"); var revision = ReadCanonicalUInt64(ref reader);
        RequireProperty(ref reader, "cameraError"); var cameraError = ReadBoundedString(ref reader, 2048);
        RequireProperty(ref reader, "desktopError"); var desktopError = ReadBoundedString(ref reader, 2048);
        return new VideoStateChanged(cameraRequested, desktopRequested, cameraActive, desktopActive, resolution, bitrate, width, height, appliedBitrate, size, position, revision, cameraError, desktopError);
    }

    private static EngineEvent ReadAudioDirection(ref Utf8JsonReader reader)
    {
        RequireProperty(ref reader,"direction"); var direction=ReadString(ref reader);
        if (direction is not ("send" or "receive")) throw new FormatException("direction");
        RequireProperty(ref reader,"codec"); var codec=ReadString(ref reader);
        if (codec is not ("aac-ld" or "aac-lc" or "g722")) throw new FormatException("codec");
        RequireProperty(ref reader,"sampleRate"); var rate=(int)ReadInt64(ref reader,8000,192000);
        RequireProperty(ref reader,"channels"); var channels=(int)ReadInt64(ref reader,1,2);
        RequireProperty(ref reader,"bitrateKbps"); var bitrate=(int)ReadInt64(ref reader,1,1024);
        return new AudioDirectionState(direction,codec,rate,channels,bitrate);
    }
    private static EngineEvent ReadAudioProfileApplied(ref Utf8JsonReader reader)
    {
        RequireProperty(ref reader, "codec"); var codec = ReadString(ref reader);
        if (codec is not ("auto" or "aac-ld" or "aac-lc" or "g722")) throw new FormatException("codec");
        RequireProperty(ref reader, "channels"); var channels = (int)ReadInt64(ref reader, 1, 2);
        RequireProperty(ref reader, "bitrateKbps"); var bitrate = (int)ReadInt64(ref reader, 1, 1024);
        RequireProperty(ref reader, "jitterMinimumMs"); var min = (int)ReadInt64(ref reader, 100, 600);
        RequireProperty(ref reader, "jitterMaximumMs"); var max = (int)ReadInt64(ref reader, min, 600);
        RequireProperty(ref reader, "jitterInitialMs"); var initial = (int)ReadInt64(ref reader, min, max);
        RequireProperty(ref reader, "sendBufferMs"); var buffer = (int)ReadInt64(ref reader, 20, 200);
        RequireProperty(ref reader, "packetMs"); var packet = (int)ReadInt64(ref reader, 0, 60);
        RequireProperty(ref reader, "negotiated"); if (ReadBoolean(ref reader)) throw new FormatException("profile-is-not-negotiation");
        return new AudioProfileApplied(codec, channels, bitrate, min, max, initial, buffer, packet);
    }
    private static EngineEvent ReadNetworkVideoApplied(ref Utf8JsonReader reader)
    {
        RequireProperty(ref reader, "fps"); var fps = (int)ReadInt64(ref reader, 2, 30);
        RequireProperty(ref reader, "bitrateKbps"); var bitrate = (int)ReadInt64(ref reader, 128, 20000);
        RequireProperty(ref reader, "content"); var content = ReadString(ref reader);
        if (content is not ("text" or "motion")) throw new FormatException("content");
        RequireProperty(ref reader, "negotiated"); if (ReadBoolean(ref reader)) throw new FormatException("profile-is-not-negotiation");
        return new NetworkVideoApplied(fps, bitrate, content);
    }
    private static double? ReadNullableMeasurement(ref Utf8JsonReader reader, string name, double maximum)
    {
        RequireProperty(ref reader, name);
        if (!reader.Read()) throw new FormatException("measurement");
        if (reader.TokenType == JsonTokenType.Null) return null;
        if (reader.TokenType != JsonTokenType.Number || !reader.TryGetDouble(out var value) || !double.IsFinite(value) || value < 0 || value > maximum) throw new FormatException("measurement");
        return value;
    }
    private static EngineEvent ReadTransportStatistics(ref Utf8JsonReader reader)
    {
        var loss=ReadNullableMeasurement(ref reader,"lossPercent",100);
        var jitter=ReadNullableMeasurement(ref reader,"jitterMs",600000);
        var queue=ReadNullableMeasurement(ref reader,"sendQueueMs",600000);
        var atx=ReadNullableMeasurement(ref reader,"audioTxKbps",1000000);
        var arx=ReadNullableMeasurement(ref reader,"audioRxKbps",1000000);
        var vtx=ReadNullableMeasurement(ref reader,"videoTxKbps",1000000);
        var vrx=ReadNullableMeasurement(ref reader,"videoRxKbps",1000000);
        double? rxLoss = null, rxJitter = null;
        var lookahead = reader;
        if (lookahead.Read() && lookahead.TokenType == JsonTokenType.PropertyName && lookahead.ValueTextEquals("receiveLossPercent"))
        {
            rxLoss=ReadNullableMeasurement(ref reader,"receiveLossPercent",100);
            rxJitter=ReadNullableMeasurement(ref reader,"receiveJitterMs",600000);
        }
        return new TransportStatistics(loss,jitter,queue,atx,arx,vtx,vrx,rxLoss,rxJitter);
    }
    private static EngineEvent ReadAudioControls(ref Utf8JsonReader reader)
    {
        RequireProperty(ref reader, "roomVolume");
        var room = checked((int)ReadInt64(ref reader, 0, 100));
        RequireProperty(ref reader, "micGain");
        var microphone = checked((int)ReadInt64(ref reader, 0, 150));
        RequireProperty(ref reader, "noiseIsolation");
        return new AudioControlsChanged(room, microphone, ReadBoolean(ref reader));
    }

    private static EngineEvent ReadAudioCodecState(ref Utf8JsonReader reader)
    {
        RequireProperty(ref reader, "requested");
        var requested = ReadAudioCodec(ref reader);
        RequireProperty(ref reader, "negotiated");
        var negotiated = ReadAudioCodec(ref reader);
        RequireProperty(ref reader, "sampleRate");
        var sampleRate = checked((int)ReadInt64(ref reader, 8000, 192000));
        RequireProperty(ref reader, "channels");
        var channels = checked((int)ReadInt64(ref reader, 1, 2));
        RequireProperty(ref reader, "bitrateKbps");
        var bitrate = checked((int)ReadInt64(ref reader, 1, 1024));
        RequireProperty(ref reader, "sendActive");
        var send = ReadBoolean(ref reader);
        RequireProperty(ref reader, "receiveActive");
        var receive = ReadBoolean(ref reader);
        if (!send && !receive) throw new FormatException("audio-codec-inactive");
        return new AudioCodecStateChanged(requested, negotiated, sampleRate, channels, bitrate, send, receive);
    }

    private static AudioCodecKind ReadAudioCodec(ref Utf8JsonReader reader) => ReadString(ref reader) switch
    {
        "aac-ld" => AudioCodecKind.AacLd,
        "aac-lc" => AudioCodecKind.AacLc,
        "g722" => AudioCodecKind.G722,
        _ => throw new FormatException("audio-codec")
    };

    private static EngineEvent ReadRecordingState(ref Utf8JsonReader reader)
    {
        RequireProperty(ref reader, "state");
        var state = ReadString(ref reader) switch
        {
            "idle" => RecordingState.Idle,
            "starting" => RecordingState.Starting,
            "recording" => RecordingState.Recording,
            "finalizing" => RecordingState.Finalizing,
            "completed" => RecordingState.Completed,
            "failed" => RecordingState.Failed,
            _ => throw new FormatException("state")
        };
        RequireProperty(ref reader, "micIncluded");
        var microphone = ReadBoolean(ref reader);
        RequireProperty(ref reader, "roomIncluded");
        var room = ReadBoolean(ref reader);
        RequireProperty(ref reader, "elapsedMs");
        var elapsed = ReadInt64(ref reader, 0, long.MaxValue);
        RequireProperty(ref reader, "fileName");
        var fileName = ReadString(ref reader);
        var paused = false;
        var peek = reader;
        if (peek.Read() && peek.TokenType == JsonTokenType.PropertyName && peek.ValueTextEquals("paused"))
        {
            reader = peek; paused = ReadBoolean(ref reader);
        }
        if (paused && state != RecordingState.Recording) throw new FormatException("recording-paused-state");
        ValidateRecordingState(state, microphone, room, elapsed, fileName);
        return new RecordingStateChanged(state, microphone, room, elapsed, fileName, paused);
    }

    private static EngineEvent ReadRecordingError(ref Utf8JsonReader reader)
    {
        RequireProperty(ref reader, "code");
        var code = ReadErrorCode(ref reader);
        RequireProperty(ref reader, "detail");
        var detail = ReadBoundedString(ref reader, 2048);
        RequireProperty(ref reader, "recoverablePart");
        var recoverable = ReadBoolean(ref reader);
        RequireProperty(ref reader, "fileName");
        var fileName = ReadString(ref reader);
        if (recoverable)
        {
            RequireFileExtension(fileName, ".part");
        }
        else if (fileName.Length != 0)
        {
            throw new FormatException("recording-error-file");
        }
        return new RecordingError(code, detail, recoverable, fileName);
    }

    private static void ValidateRecordingState(RecordingState state, bool microphone, bool room, long elapsed, string fileName)
    {
        switch (state)
        {
            case RecordingState.Idle when microphone || room || elapsed != 0 || fileName.Length != 0:
            case RecordingState.Starting when elapsed != 0 || fileName.Length != 0:
            case RecordingState.Recording when fileName.Length != 0:
            case RecordingState.Finalizing when fileName.Length != 0:
                throw new FormatException("recording-state-fields");
            case RecordingState.Completed:
                RequireFileExtension(fileName, ".mp4");
                break;
            case RecordingState.Failed when fileName.Length != 0:
                RequireFileExtension(fileName, ".part");
                break;
        }
    }

    private static void RequireFileExtension(string fileName, string extension)
    {
        if (!IsSafeRecordingFileName(fileName) || !fileName.EndsWith(extension, StringComparison.OrdinalIgnoreCase))
        {
            throw new FormatException("file-name");
        }
    }

    private static VideoSource ReadSource(ref Utf8JsonReader reader) => ReadString(ref reader) switch
    {
        "camera" => VideoSource.Camera,
        "desktop" => VideoSource.Desktop,
        _ => throw new FormatException("source")
    };

    private static string ReadErrorCode(ref Utf8JsonReader reader)
    {
        var value = ReadString(ref reader);
        if (value.Length is < 1 or > 64 || !IsLowerAlphaNumeric(value[0]) ||
            value.Any(character => character is not (>= 'a' and <= 'z') and not (>= '0' and <= '9') and not '-'))
        {
            throw new FormatException("error-code");
        }
        return value;
    }

    private static string ReadBoundedString(ref Utf8JsonReader reader, int maximumScalars)
    {
        var value = ReadString(ref reader);
        if (!IsValidScalarString(value, maximumScalars))
        {
            throw new FormatException("string");
        }
        return value;
    }

    private static bool IsValidScalarString(string value, int maximumScalars)
    {
        var count = 0;
        var remaining = value.AsSpan();
        while (!remaining.IsEmpty)
        {
            if (Rune.DecodeFromUtf16(remaining, out _, out var consumed) != OperationStatus.Done)
            {
                return false;
            }
            remaining = remaining[consumed..];
            count++;
            if (count > maximumScalars)
            {
                return false;
            }
        }
        return true;
    }

    private static bool IsLowerAlphaNumeric(char value) => value is >= 'a' and <= 'z' or >= '0' and <= '9';

    private static ulong ReadCanonicalUInt64(ref Utf8JsonReader reader)
    {
        var value = ReadString(ref reader);
        if (value.Length == 0 || value[0] == '0' || value.Any(character => character is < '0' or > '9') ||
            !ulong.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out var result) || result == 0)
        {
            throw new FormatException("unsigned-decimal");
        }
        return result;
    }

    private static long ReadInt64(ref Utf8JsonReader reader, long minimum, long maximum)
    {
        Require(ref reader, JsonTokenType.Number);
        var token = reader.ValueSpan;
        if (token.Length == 0 || token.Contains((byte)'.') || token.Contains((byte)'e') || token.Contains((byte)'E') ||
            !reader.TryGetInt64(out var value) || value < minimum || value > maximum)
        {
            throw new FormatException("integer");
        }
        return value;
    }

    private static int? ReadNullableInt(ref Utf8JsonReader reader, int minimum, int maximum)
    {
        if (!reader.Read()) throw new FormatException("integer");
        if (reader.TokenType == JsonTokenType.Null) return null;
        if (reader.TokenType != JsonTokenType.Number || !reader.TryGetInt32(out var value) || value < minimum || value > maximum) throw new FormatException("integer");
        return value;
    }

    private static double ReadDouble(ref Utf8JsonReader reader, double minimum, double maximum)
    {
        Require(ref reader, JsonTokenType.Number);
        if (!reader.TryGetDouble(out var value) || !double.IsFinite(value) || value < minimum || value > maximum)
        {
            throw new FormatException("number");
        }
        return value;
    }

    private static bool ReadBoolean(ref Utf8JsonReader reader)
    {
        if (!reader.Read() || reader.TokenType is not (JsonTokenType.True or JsonTokenType.False))
        {
            throw new FormatException("boolean");
        }
        return reader.TokenType == JsonTokenType.True;
    }

    private static string ReadString(ref Utf8JsonReader reader)
    {
        Require(ref reader, JsonTokenType.String);
        var value = reader.GetString() ?? throw new FormatException("null-string");
        if (!IsValidScalarString(value, int.MaxValue))
        {
            throw new FormatException("invalid-scalar");
        }
        return value;
    }

    private static void RequireProperty(ref Utf8JsonReader reader, string name)
    {
        Require(ref reader, JsonTokenType.PropertyName);
        if (!reader.ValueTextEquals(name))
        {
            throw new FormatException("field-order");
        }
    }

    private static void Require(ref Utf8JsonReader reader, JsonTokenType token)
    {
        if (!reader.Read() || reader.TokenType != token)
        {
            throw new FormatException("token");
        }
    }

    private static bool IsCompact(ReadOnlySpan<byte> json)
    {
        var inString = false;
        var escaped = false;
        foreach (var value in json)
        {
            if (inString)
            {
                if (escaped)
                {
                    escaped = false;
                }
                else if (value == (byte)'\\')
                {
                    escaped = true;
                }
                else if (value == (byte)'"')
                {
                    inString = false;
                }
                continue;
            }
            if (value == (byte)'"')
            {
                inString = true;
            }
            else if (value is (byte)' ' or (byte)'\t' or (byte)'\r' || value < 0x20)
            {
                return false;
            }
        }
        return !inString && !escaped;
    }

    private static string MapReason(Exception error) => error switch
    {
        DecoderFallbackException => "invalid-utf8",
        JsonException => "invalid-json",
        FormatException format when format.Message.Length <= 64 => format.Message,
        _ => "invalid-record"
    };
}
