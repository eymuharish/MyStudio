namespace TeleStudio.Desktop.Audio;

public interface IAudioDiagnostics : IAsyncDisposable
{
    IReadOnlyList<AudioDeviceInfo> GetCaptureDevices();

    Task<MicrophoneTestResult> RecordTestAsync(
        string? deviceId,
        TimeSpan duration,
        CancellationToken cancellationToken);

    Task PlayAsync(string wavePath, string? speakerId, CancellationToken cancellationToken);
}
