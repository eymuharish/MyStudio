namespace TeleStudio.Desktop.Audio;

public sealed record AudioDeviceInfo(string Id, string Name, bool IsDefault)
{
    public override string ToString() => Name;
}

public sealed record MicrophoneTestResult(
    string DeviceId,
    string DeviceName,
    double PeakDbFs,
    double RmsDbFs,
    bool NonSilent,
    string WavePath);
