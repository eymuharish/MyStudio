using TeleStudio.Contracts;

namespace TeleStudio.Settings;

public sealed class AppSettings : IEquatable<AppSettings>
{
    public static AppSettings Default => new();

    public string Language { get; init; } = "en";
    public string? CameraId { get; init; }
    public string? MicrophoneId { get; init; }
    public string? SpeakerId { get; init; }
    public int RtpPortStart { get; init; } = 40000;
    public int RtpPortEnd { get; init; } = 40199;
    public List<string> RecentDestinations { get; init; } = [];
    public CodecPreference Codec { get; init; } = CodecPreference.Auto;
    public QualityMode Quality { get; init; } = QualityMode.Auto;
    public VideoResolution VideoResolution { get; init; } = VideoResolution.Auto;
    public int? VideoBitrateKbps { get; init; }
    public int OpalVideoDefaultsVersion { get; init; }
    public CameraOverlaySize CameraOverlaySize { get; init; } = CameraOverlaySize.Medium;
    public CameraOverlayPosition CameraOverlayPosition { get; init; } = CameraOverlayPosition.BottomRight;
    public EndpointProfile Endpoint { get; init; } = EndpointProfile.Auto;
    public string LecturerName { get; init; } = "EssaStudio";
    public int AudioDscp { get; init; } = 32;
    public int VideoDscp { get; init; } = 34;
    public int SignalingDscp { get; init; } = 24;
    public int RoomVolume { get; init; } = 100;
    public int MicrophoneGain { get; init; } = 100;
    public bool NoiseIsolation { get; init; } = true;
    public NetworkConnectionMode NetworkMode { get; init; } = NetworkConnectionMode.ExternalVpn;
    public SipEngineKind SipEngine { get; init; } = SipEngineKind.Opal;

    public AppSettings WithRecentDestination(string destination)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(destination);
        var normalized = destination.Trim();
        var recent = new List<string>(capacity: 10) { normalized };
        recent.AddRange(RecentDestinations.Where(item => !string.Equals(item, normalized, StringComparison.OrdinalIgnoreCase)).Take(9));

        return new AppSettings
        {
            Language = Language,
            CameraId = CameraId,
            MicrophoneId = MicrophoneId,
            SpeakerId = SpeakerId,
            RtpPortStart = RtpPortStart,
            RtpPortEnd = RtpPortEnd,
            RecentDestinations = recent,
            Codec = Codec,
            Quality = Quality,
            VideoResolution = VideoResolution,
            VideoBitrateKbps = VideoBitrateKbps,
            OpalVideoDefaultsVersion = OpalVideoDefaultsVersion,
            CameraOverlaySize = CameraOverlaySize,
            CameraOverlayPosition = CameraOverlayPosition,
            Endpoint = Endpoint,
            LecturerName = LecturerName,
            AudioDscp = AudioDscp,
            VideoDscp = VideoDscp,
            SignalingDscp = SignalingDscp,
            RoomVolume = RoomVolume,
            MicrophoneGain = MicrophoneGain,
            NoiseIsolation = NoiseIsolation,
            NetworkMode = NetworkMode,
            SipEngine = SipEngine,
        };
    }

    public bool Equals(AppSettings? other) =>
        other is not null &&
        Language == other.Language &&
        CameraId == other.CameraId &&
        MicrophoneId == other.MicrophoneId &&
        SpeakerId == other.SpeakerId &&
        RtpPortStart == other.RtpPortStart &&
        RtpPortEnd == other.RtpPortEnd &&
        RecentDestinations.SequenceEqual(other.RecentDestinations, StringComparer.Ordinal) &&
        Codec == other.Codec &&
        Quality == other.Quality &&
        VideoResolution == other.VideoResolution && VideoBitrateKbps == other.VideoBitrateKbps &&
        OpalVideoDefaultsVersion == other.OpalVideoDefaultsVersion &&
        CameraOverlaySize == other.CameraOverlaySize && CameraOverlayPosition == other.CameraOverlayPosition &&
        Endpoint == other.Endpoint &&
        LecturerName == other.LecturerName &&
        AudioDscp == other.AudioDscp &&
        VideoDscp == other.VideoDscp &&
        SignalingDscp == other.SignalingDscp &&
        RoomVolume == other.RoomVolume &&
        MicrophoneGain == other.MicrophoneGain &&
        NoiseIsolation == other.NoiseIsolation &&
        NetworkMode == other.NetworkMode && SipEngine == other.SipEngine;

    public override bool Equals(object? obj) => Equals(obj as AppSettings);

    public override int GetHashCode()
    {
        var hash = new HashCode();
        hash.Add(Language);
        hash.Add(CameraId);
        hash.Add(MicrophoneId);
        hash.Add(SpeakerId);
        hash.Add(RtpPortStart);
        hash.Add(RtpPortEnd);
        foreach (var destination in RecentDestinations)
        {
            hash.Add(destination);
        }
        hash.Add(Codec);
        hash.Add(Quality);
        hash.Add(OpalVideoDefaultsVersion);
        hash.Add(VideoResolution); hash.Add(VideoBitrateKbps); hash.Add(CameraOverlaySize); hash.Add(CameraOverlayPosition);
        hash.Add(Endpoint);
        hash.Add(LecturerName);
        hash.Add(AudioDscp);
        hash.Add(VideoDscp);
        hash.Add(SignalingDscp);
        hash.Add(RoomVolume);
        hash.Add(MicrophoneGain);
        hash.Add(NoiseIsolation);
        hash.Add(NetworkMode);
        hash.Add(SipEngine);
        return hash.ToHashCode();
    }
}
