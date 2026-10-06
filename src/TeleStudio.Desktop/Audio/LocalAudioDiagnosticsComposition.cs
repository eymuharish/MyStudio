namespace TeleStudio.Desktop.Audio;

public sealed class LocalAudioDiagnosticsComposition
{
    public IAudioDiagnostics Create() => new WasapiAudioDiagnostics();
}
