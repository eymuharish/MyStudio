using TeleStudio.Contracts;

namespace TeleStudio.Settings;

public static class OpalEditionDefaults
{
    public static AppSettings Settings(EngineFamily family) => UpgradeVideoSettings(new());

    public static AppSettings UpgradeVideoSettings(AppSettings settings) => settings.OpalVideoDefaultsVersion >= 1 ? settings
        : AppSettingsCopy.CopyVideo(settings, VideoResolution.Fhd, 4000, 1);

    public static ConnectionTuning Tuning(EngineFamily family) => UpgradeVideoDefaults(UpgradeAudioDefaults(new(), family));

    public static ConnectionTuning UpgradeVideoDefaults(ConnectionTuning tuning) => tuning.OpalVideoDefaultsVersion >= 1 ? tuning : tuning with
    {
        OpalVideoDefaultsVersion = 1, CameraResolution = VideoResolution.Fhd,
        ScreenResolution = VideoResolution.Fhd, VideoCeilingKbps = 4000, CameraFps = 30, ScreenFps = 30
    };

    public static ConnectionTuning UpgradeAudioDefaults(ConnectionTuning tuning, EngineFamily family)
        => tuning.OpalAudioDefaultsVersion >= 1 ? tuning : tuning with
        {
            OpalAudioDefaultsVersion = 1, AacEnabled = true,
            AudioCodec = family == EngineFamily.Sony ? AudioSelection.AacLc : AudioSelection.AacLd,
            AudioChannels = 2, AudioKbps = family == EngineFamily.Sony ? 192 : 128, PacketMs = 0
        };
}
