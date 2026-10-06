using System.Text.Json.Serialization;
using System.Text;

namespace TeleStudio.Contracts;

public enum CodecPreference { Auto, H264, H265 }
public enum QualityMode { Auto, Presentation, Motion, Maximum }
public enum NetworkConnectionMode { Building, ExternalVpn }
public enum VideoResolution { Auto, Sd, Hd, Fhd }
public enum CameraOverlaySize { Small, Medium, Large }
public enum CameraOverlayPosition { TopLeft, TopRight, BottomLeft, BottomRight }
public enum SharingTransmissionMode { Auto, MainOnly, MainAndPresentation }
public enum RecordingState { Idle, Starting, Recording, Finalizing, Completed, Failed }

[JsonPolymorphic(TypeDiscriminatorPropertyName = "type")]
[JsonDerivedType(typeof(Dial), "dial")]
[JsonDerivedType(typeof(Answer), "answer")]
[JsonDerivedType(typeof(Reject), "reject")]
[JsonDerivedType(typeof(HangUp), "hangUp")]
[JsonDerivedType(typeof(SetCallHeld), "setCallHeld")]
[JsonDerivedType(typeof(SendDtmf), "sendDtmf")]
[JsonDerivedType(typeof(SetMediaPreferences), "setMediaPreferences")]
[JsonDerivedType(typeof(SetNetworkMode), "setNetworkMode")]
[JsonDerivedType(typeof(SetConnectionTuning), "setConnectionTuning")]
[JsonDerivedType(typeof(SetVideoPreferences), "setVideoPreferences")]
[JsonDerivedType(typeof(SetCameraEnabled), "setCameraEnabled")]
[JsonDerivedType(typeof(SetDesktopEnabled), "setDesktopEnabled")]
[JsonDerivedType(typeof(SetCameraOverlay), "setCameraOverlay")]
[JsonDerivedType(typeof(SwitchSource), "switchSource")]
[JsonDerivedType(typeof(SetMuted), "setMuted")]
[JsonDerivedType(typeof(SetAudioControls), "setAudioControls")]
[JsonDerivedType(typeof(SetMicrophoneMuted), "set-microphone-muted")]
[JsonDerivedType(typeof(SetSystemAudioEnabled), "set-system-audio-enabled")]
[JsonDerivedType(typeof(SetRoomAudioMuted), "set-room-audio-muted")]
[JsonDerivedType(typeof(SetRoomMixer), "set-room-mixer")]
[JsonDerivedType(typeof(StartLectureRecording), "startLectureRecording")]
[JsonDerivedType(typeof(StopLectureRecording), "stopLectureRecording")]
[JsonDerivedType(typeof(SetLectureRecordingPaused), "setLectureRecordingPaused")]
public abstract record EngineCommand;

public sealed record Dial(string Destination, CallProtocol Protocol, string LecturerName) : EngineCommand;
public sealed record Answer : EngineCommand;
public sealed record Reject : EngineCommand;
public sealed record HangUp : EngineCommand;
public sealed record SetCallHeld(bool Held) : EngineCommand;
public sealed record SendDtmf : EngineCommand
{
    public SendDtmf(string digits)
    {
        if (string.IsNullOrEmpty(digits) || digits.Length > 32 || digits.Any(c => !"0123456789*#ABCD".Contains(c)))
            throw new ArgumentException("DTMF accepts 1–32 digits: 0–9, *, #, A–D.", nameof(digits));
        Digits = digits;
    }
    public string Digits { get; }
}
public sealed record SetMediaPreferences(CodecPreference Codec, QualityMode Quality, EndpointProfile Endpoint) : EngineCommand;
public sealed record SetNetworkMode(NetworkConnectionMode Mode) : EngineCommand;
public sealed record SetVideoPreferences : EngineCommand
{
    public SetVideoPreferences(VideoResolution resolution, int? bitrateKbps)
    {
        if (!Enum.IsDefined(resolution)) throw new ArgumentOutOfRangeException(nameof(resolution));
        if (bitrateKbps is < 128 or > 20000) throw new ArgumentOutOfRangeException(nameof(bitrateKbps));
        Resolution = resolution;
        BitrateKbps = bitrateKbps;
    }
    public VideoResolution Resolution { get; }
    public int? BitrateKbps { get; }
}
public sealed record SetCameraEnabled(bool Enabled, string? DeviceId = null) : EngineCommand;
public sealed record SetDesktopEnabled(bool Enabled, string? DisplayId = null, SharingTransmissionMode TransmissionMode = SharingTransmissionMode.Auto) : EngineCommand;
public sealed record SetCameraOverlay(CameraOverlaySize Size, CameraOverlayPosition Position) : EngineCommand;
public sealed record SwitchSource(VideoSource Source, string? DisplayId = null) : EngineCommand;
public sealed record SetMuted(bool IsMuted) : EngineCommand;
public sealed record SetAudioControls(int RoomVolume, int MicrophoneGain, bool NoiseIsolation) : EngineCommand;
public sealed record SetEchoCancellation(bool Enabled) : EngineCommand;
public sealed record SetMicrophoneMuted(bool IsMuted) : EngineCommand;
public sealed record SetSystemAudioEnabled(bool Enabled) : EngineCommand;
public sealed record SetRoomAudioMuted(bool IsMuted) : EngineCommand;
public sealed record SetRoomMixer(int MasterPercent, int BassDb, int VoiceDb, int TrebleDb, int NoiseReductionDb = 0) : EngineCommand;
public sealed record StartLectureRecording(string SourceId, bool IncludeMicrophone, bool IncludeRoomAudio) : EngineCommand;
public sealed record StopLectureRecording : EngineCommand;
public sealed record SetLectureRecordingPaused(bool Paused) : EngineCommand;

[JsonPolymorphic(TypeDiscriminatorPropertyName = "type")]
[JsonDerivedType(typeof(IncomingCall), "incomingCall")]
[JsonDerivedType(typeof(CallProgress), "callProgress")]
[JsonDerivedType(typeof(CallConnected), "callConnected")]
[JsonDerivedType(typeof(LocalSourceChanged), "localSourceChanged")]
[JsonDerivedType(typeof(CallEnding), "callEnding")]
[JsonDerivedType(typeof(CallEnded), "callEnded")]
[JsonDerivedType(typeof(CallFailed), "callFailed")]
[JsonDerivedType(typeof(MuteChanged), "muteChanged")]
[JsonDerivedType(typeof(MediaCapabilitiesChanged), "mediaCapabilitiesChanged")]
[JsonDerivedType(typeof(CallHoldChanged), "callHoldChanged")]
[JsonDerivedType(typeof(ContentSharingStateChanged), "contentSharingStateChanged")]
[JsonDerivedType(typeof(SipRegistrationStateChanged), "sipRegistrationStateChanged")]
[JsonDerivedType(typeof(SipSecurityStateChanged), "sipSecurityStateChanged")]
[JsonDerivedType(typeof(SipConnectionHealthChanged), "sipConnectionHealthChanged")]
[JsonDerivedType(typeof(SipCallModeChanged), "sipCallModeChanged")]
[JsonDerivedType(typeof(SipAudioUnavailable), "sipAudioUnavailable")]
[JsonDerivedType(typeof(SipOutboundVideoProofChanged), "sipOutboundVideoProofChanged")]
[JsonDerivedType(typeof(OpalVideoTransportChanged), "opalVideoTransportChanged")]
[JsonDerivedType(typeof(MediaStatsChanged), "mediaStatsChanged")]
[JsonDerivedType(typeof(MediaError), "mediaError")]
[JsonDerivedType(typeof(RemoteSurfaceChanged), "remoteSurfaceChanged")]
[JsonDerivedType(typeof(LocalSurfaceChanged), "localSurfaceChanged")]
[JsonDerivedType(typeof(VideoStateChanged), "videoStateChanged")]
[JsonDerivedType(typeof(AudioControlsChanged), "audioControlsChanged")]
[JsonDerivedType(typeof(AudioCodecStateChanged), "audioCodecStateChanged")]
[JsonDerivedType(typeof(AudioProfileApplied), "audioProfileApplied")]
[JsonDerivedType(typeof(NetworkVideoApplied), "networkVideoApplied")]
[JsonDerivedType(typeof(AudioDirectionState), "audioDirectionState")]
[JsonDerivedType(typeof(TransportStatistics), "transportStatistics")]
[JsonDerivedType(typeof(MicrophoneMuteChanged), "microphone-mute-changed")]
[JsonDerivedType(typeof(SystemAudioChanged), "system-audio-changed")]
[JsonDerivedType(typeof(RoomAudioMuteChanged), "room-audio-mute-changed")]
[JsonDerivedType(typeof(RoomMixerChanged), "room-mixer-changed")]
[JsonDerivedType(typeof(RoomAudioMeterChanged), "room-audio-meter")]
[JsonDerivedType(typeof(RecordingStateChanged), "recordingStateChanged")]
[JsonDerivedType(typeof(RecordingError), "recordingError")]
public abstract record EngineEvent;

public enum AudioCodecKind { AacLd, AacLc, G722 }

public sealed record IncomingCall(string RemoteAddress, CallProtocol Protocol) : EngineEvent;
public sealed record CallProgress(string RemoteAddress, CallProtocol Protocol, CallState Stage) : EngineEvent;
public sealed record CallConnected(string RemoteAddress, CallProtocol Protocol, int VideoWidth, int VideoHeight, int FramesPerSecond) : EngineEvent;
public sealed record LocalSourceChanged(VideoSource Source) : EngineEvent;
public sealed record CallEnding : EngineEvent;
public sealed record CallEnded(string Reason) : EngineEvent;
public sealed record CallFailed(string ErrorCode, string? Detail = null) : EngineEvent;
public sealed record MuteChanged(bool IsMuted) : EngineEvent;
public sealed record CallHoldChanged(bool Held) : EngineEvent;
public sealed record ContentSharingStateChanged(string Mode, bool Active, string Detail) : EngineEvent;
public sealed record SipRegistrationStateChanged(string State, string Detail) : EngineEvent;
public sealed record SipSecurityStateChanged(string Transport, string MediaEncryption) : EngineEvent;
public sealed record SipSessionTimerChanged(int SessionIntervalSeconds, int RefreshAfterSeconds, bool LocalRefresher) : EngineEvent;
public sealed record SipCallModeChanged(string Mode) : EngineEvent;
public sealed record SipAudioUnavailable(string Reason, string Detail) : EngineEvent;
public sealed record SipOutboundVideoProofChanged(long SourceFrames, long EncodedFrameEnds, double? EncoderFps,
    long RtpPacketsSent, long RtpBytesSent, bool PeerReportAvailable, long? PeerExtendedSequence,
    double? PeerLossPercent, long? PeerCumulativeLoss, double? PeerJitterMs, uint? SenderSsrc, uint? PeerReportedSsrc) : EngineEvent;
public sealed record OpalVideoTransportChanged(int SessionId, string Direction, string Role, string Codec,
    int PayloadType, int Width, int Height, int? PacketizationMode, string ProfileLevelId,
    string LocalRtp, string RemoteRtp, long PacketsSent, long OctetsSent, long MarkersSent,
    long PacketsReceived, long OctetsReceived, long? RemotePacketsLost, uint SenderSsrc) : EngineEvent;
public sealed record SipConnectionHealthChanged(double? AudioTxKbps, double? AudioRxKbps, double? VideoTxKbps, double? VideoRxKbps,
    double? LossPercent, double? JitterBufferMs, double? RoundTripMs, long CapturedSamples, long DecodedAudioSamples,
    long SentFrames, long ReceivedFrames, long DecodedNonzeroSamples, long DeviceSubmittedNonzeroSamples) : EngineEvent;
public sealed record MediaCapabilitiesChanged(
    bool SourceControl,
    bool MuteControl,
    bool Stats,
    bool RemoteSurface,
    bool AudioControl,
    bool Recording,
    bool SystemAudioControl = false,
    bool RoomMixerControl = false,
    bool VideoControl = false,
    bool HoldControl = false,
    bool DtmfControl = false) : EngineEvent;
public sealed record MediaStatsChanged(CodecPreference Codec, VideoSource Source, int VideoWidth, int VideoHeight, int FramesPerSecond, int BitrateKbps, double LossPercent, double JitterMs, double OneWayLatencyMs, long DroppedFrames) : EngineEvent;
public sealed record MediaError(string Code, string Detail) : EngineEvent;
public sealed record RemoteSurfaceChanged(ulong HostHandleValue, int VideoWidth, int VideoHeight, ulong Epoch) : EngineEvent;
public sealed record LocalSurfaceChanged(ulong HostHandleValue, int VideoWidth, int VideoHeight, ulong Epoch) : EngineEvent;
public sealed record VideoStateChanged(bool CameraRequested, bool DesktopRequested, bool CameraActive, bool DesktopActive,
    VideoResolution Resolution, int? BitrateKbps, int AppliedWidth, int AppliedHeight, int AppliedBitrateKbps,
    CameraOverlaySize OverlaySize, CameraOverlayPosition OverlayPosition, ulong Revision, string CameraError, string DesktopError) : EngineEvent;
public sealed record AudioControlsChanged(int RoomVolume, int MicrophoneGain, bool NoiseIsolation) : EngineEvent;
public sealed record EchoCancellationChanged(bool Available, bool Enabled) : EngineEvent;
public sealed record AudioCodecStateChanged(
    AudioCodecKind Requested,
    AudioCodecKind Negotiated,
    int SampleRate,
    int Channels,
    int BitrateKbps,
    bool SendActive,
    bool ReceiveActive) : EngineEvent;
public sealed record MicrophoneMuteChanged(bool IsMuted) : EngineEvent;
public sealed record SystemAudioChanged(bool Enabled) : EngineEvent;
public sealed record RoomAudioMuteChanged(bool IsMuted) : EngineEvent;
public sealed record RoomMixerChanged(int MasterPercent, int BassDb, int VoiceDb, int TrebleDb, int NoiseReductionDb = 0) : EngineEvent;
public sealed record RoomAudioMeterChanged(
    int PreLimiterPeakQ15,
    int PostLimiterPeakQ15,
    int LimiterGainReductionQ15,
    bool LimiterActive) : EngineEvent;
public sealed record RecordingStateChanged(RecordingState State, bool MicIncluded, bool RoomIncluded, long ElapsedMs, string FileName, bool Paused = false) : EngineEvent;
public sealed record RecordingError(string Code, string Detail, bool RecoverablePart, string FileName) : EngineEvent;

public static class LecturerNameCodec
{
    public static string Normalize(string value)
    {
        ArgumentNullException.ThrowIfNull(value);
        var builder = new StringBuilder(value.Length);
        var pendingSpace = false;
        foreach (var character in value)
        {
            if (character == ' ')
            {
                pendingSpace = builder.Length > 0;
                continue;
            }
            if (!IsAllowed(character))
            {
                throw new ArgumentException("Lecturer name contains an unsupported character.", nameof(value));
            }
            if (pendingSpace)
            {
                builder.Append(' ');
                pendingSpace = false;
            }
            builder.Append(character);
        }

        if (builder.Length is < 2 or > 64)
        {
            throw new ArgumentException("Lecturer name must contain 2 to 64 characters.", nameof(value));
        }
        return builder.ToString();
    }

    public static string Encode(string value)
    {
        var normalized = Normalize(value);
        return Convert.ToBase64String(new UTF8Encoding(false, true).GetBytes(normalized))
            .TrimEnd('=')
            .Replace('+', '-')
            .Replace('/', '_');
    }

    private static bool IsAllowed(char value) =>
        value is >= 'A' and <= 'Z' or >= 'a' and <= 'z' or >= '0' and <= '9' or '.' or '\'' or '-';
}
