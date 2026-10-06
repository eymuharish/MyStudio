using System.IO;
using System.Text.Json;

namespace TeleStudio.Desktop.Audio;

public sealed record MicrophoneEvidence
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        WriteIndented = true
    };

    private MicrophoneEvidence(
        string deviceName,
        double peakDbFs,
        double rmsDbFs,
        double durationSeconds,
        bool nonSilent,
        string playbackStatus,
        bool userConfirmed)
    {
        DeviceName = deviceName;
        PeakDbFs = peakDbFs;
        RmsDbFs = rmsDbFs;
        DurationSeconds = durationSeconds;
        NonSilent = nonSilent;
        PlaybackStatus = playbackStatus;
        UserConfirmed = userConfirmed;
    }

    public string DeviceName { get; private init; }
    public double PeakDbFs { get; private init; }
    public double RmsDbFs { get; private init; }
    public double DurationSeconds { get; private init; }
    public bool NonSilent { get; private init; }
    public string PlaybackStatus { get; private init; }
    public bool UserConfirmed { get; private init; }

    public static MicrophoneEvidence FromResult(MicrophoneTestResult result, TimeSpan duration)
    {
        ArgumentNullException.ThrowIfNull(result);
        if (!double.IsFinite(result.PeakDbFs) || !double.IsFinite(result.RmsDbFs))
        {
            throw new InvalidDataException("Microphone evidence levels must be finite.");
        }

        if (!double.IsFinite(duration.TotalSeconds) || duration <= TimeSpan.Zero)
        {
            throw new InvalidDataException("Microphone evidence duration must be finite and positive.");
        }

        return new(
            result.DeviceName,
            result.PeakDbFs,
            result.RmsDbFs,
            duration.TotalSeconds,
            result.NonSilent,
            "STARTED",
            false);
    }

    public MicrophoneEvidence MarkPlaybackCompleted()
    {
        if (!string.Equals(PlaybackStatus, "STARTED", StringComparison.Ordinal))
        {
            throw new InvalidOperationException("Playback can complete only after it has started.");
        }

        return this with { PlaybackStatus = "COMPLETED", UserConfirmed = false };
    }

    public MicrophoneEvidence ConfirmPlaybackAudible()
    {
        if (!string.Equals(PlaybackStatus, "COMPLETED", StringComparison.Ordinal))
        {
            throw new InvalidOperationException("Audibility can be confirmed only after playback completes.");
        }

        return this with { PlaybackStatus = "USER_CONFIRMED", UserConfirmed = true };
    }

    public string ToJson() => JsonSerializer.Serialize(this, JsonOptions);
}
