using TeleStudio.Contracts;

namespace TeleStudio.Settings;

/// <summary>Copies preferences without depending on a window or a signaling engine.</summary>
public static class AppSettingsCopy
{
    public static AppSettings Copy(AppSettings source, string lecturer, string language,
        NetworkConnectionMode? networkMode = null, SipEngineKind? sipEngine = null,
        string? microphoneId = null, string? speakerId = null) =>
        CopyCore(source, lecturer, language, sipEngine, microphoneId, speakerId);

    private static AppSettings CopyCore(AppSettings source, string lecturer, string language,
        SipEngineKind? sipEngine = null, string? microphoneId = null, string? speakerId = null,
        VideoResolution? videoResolution = null, int? videoBitrateKbps = null, int? videoDefaultsVersion = null, CodecPreference? codec = null) => new()
    {
        Language = language, CameraId = source.CameraId, MicrophoneId = microphoneId ?? source.MicrophoneId, SpeakerId = speakerId ?? source.SpeakerId,
        RtpPortStart = source.RtpPortStart, RtpPortEnd = source.RtpPortEnd, RecentDestinations = source.RecentDestinations.ToList(),
        Codec = codec ?? CodecPreference.H264, Quality = source.Quality, Endpoint = source.Endpoint, LecturerName = lecturer,
        VideoResolution = videoResolution ?? source.VideoResolution, VideoBitrateKbps = videoResolution is null ? source.VideoBitrateKbps : videoBitrateKbps,
        OpalVideoDefaultsVersion = videoDefaultsVersion ?? source.OpalVideoDefaultsVersion,
        CameraOverlaySize = source.CameraOverlaySize, CameraOverlayPosition = source.CameraOverlayPosition,
        AudioDscp = source.AudioDscp, VideoDscp = source.VideoDscp, SignalingDscp = source.SignalingDscp,
        RoomVolume = source.RoomVolume, MicrophoneGain = source.MicrophoneGain, NoiseIsolation = source.NoiseIsolation,
        NetworkMode = NetworkConnectionMode.ExternalVpn, SipEngine = sipEngine ?? source.SipEngine
    };

    public static AppSettings CopyVideo(AppSettings source, VideoResolution resolution, int? bitrate, int defaultsVersion) =>
        CopyCore(source, source.LecturerName, source.Language, videoResolution: resolution, videoBitrateKbps: bitrate, videoDefaultsVersion: defaultsVersion, codec: source.Codec);
}
