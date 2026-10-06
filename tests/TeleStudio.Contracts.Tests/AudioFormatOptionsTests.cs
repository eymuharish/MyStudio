using TeleStudio.Contracts;

namespace TeleStudio.Contracts.Tests;

public sealed class AudioFormatOptionsTests
{
    [Theory]
    [InlineData(EngineFamily.Cisco, AudioSelection.AacLd, 2, 128)]
    [InlineData(EngineFamily.Sony, AudioSelection.AacLc, 1, 96)]
    public void Sip_accepts_both_negotiable_channel_layouts(EngineFamily family, AudioSelection codec, int channels, int kbps)
    {
        new ConnectionTuning { AudioCodec=codec, AudioChannels=channels, AudioKbps=kbps }.Validate(family, CallProtocol.Sip);
    }
    [Fact]
    public void Cisco_h323_exposes_stereo_and_serializes_real_engine_request()
    {
        var stereo = Assert.Single(AudioFormatOptions.For(AudioSelection.AacLd, CallProtocol.H323), f => f.Channels == 2);
        Assert.Equal(128, stereo.Kbps);
        var tuning = new ConnectionTuning { AudioCodec = AudioSelection.AacLd, AudioChannels = stereo.Channels, AudioKbps = stereo.Kbps };
        tuning.Validate(EngineFamily.Cisco, CallProtocol.H323);
        Assert.StartsWith("audio-profile aac-ld 2 128 ", tuning.AudioCommand);
    }

    [Fact]
    public void Sip_exposes_stereo_preference_and_matching_mono_fallbacks()
    {
        Assert.Equal(new[] {new AudioFormatOption(2, 128), new AudioFormatOption(1, 64)}, AudioFormatOptions.For(AudioSelection.AacLd, CallProtocol.Sip));
        Assert.Equal(new[] {new AudioFormatOption(2, 192), new AudioFormatOption(1, 96)}, AudioFormatOptions.For(AudioSelection.AacLc, CallProtocol.Sip));
    }
}
