using TeleStudio.Contracts;
using TeleStudio.Settings;

namespace TeleStudio.Settings.Tests;

public sealed class AppSettingsCopyTests
{
    [Fact]
    public void Changing_name_and_microphone_preserves_the_selected_media_and_audio_settings()
    {
        var source = new AppSettings { Language = "en", LecturerName = "Old Name", CameraId = "camera", MicrophoneId = "mic",
            SpeakerId = "speaker", RtpPortStart = 43000, RtpPortEnd = 43199, RecentDestinations = ["10.2.3.4"],
            VideoResolution = VideoResolution.Fhd, VideoBitrateKbps = 4500, CameraOverlaySize = CameraOverlaySize.Large,
            CameraOverlayPosition = CameraOverlayPosition.BottomLeft, Endpoint = EndpointProfile.SonyXg80, Quality = QualityMode.Motion,
            RoomVolume = 43, MicrophoneGain = 121, NoiseIsolation = false, AudioDscp = 1, VideoDscp = 2, SignalingDscp = 3 };
        var copied = AppSettingsCopy.Copy(source, "New Name", "ar", NetworkConnectionMode.ExternalVpn, SipEngineKind.Opal, "new-mic");
        var expected = new AppSettings { Language = "ar", LecturerName = "New Name", CameraId = "camera", MicrophoneId = "new-mic",
            SpeakerId = "speaker", RtpPortStart = 43000, RtpPortEnd = 43199, RecentDestinations = ["10.2.3.4"], Codec = CodecPreference.H264,
            VideoResolution = VideoResolution.Fhd, VideoBitrateKbps = 4500, CameraOverlaySize = CameraOverlaySize.Large,
            CameraOverlayPosition = CameraOverlayPosition.BottomLeft, Endpoint = EndpointProfile.SonyXg80, Quality = QualityMode.Motion,
            RoomVolume = 43, MicrophoneGain = 121, NoiseIsolation = false, AudioDscp = 1, VideoDscp = 2, SignalingDscp = 3,
            NetworkMode = NetworkConnectionMode.ExternalVpn, SipEngine = SipEngineKind.Opal };
        Assert.Equal(expected, copied);
        copied.RecentDestinations.Add("10.5.6.7");
        Assert.Equal(new[] { "10.2.3.4" }, source.RecentDestinations);
    }
}
