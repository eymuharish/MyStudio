namespace TeleStudio.Contracts;

public sealed record AudioFormatOption(int Channels, int Kbps)
{
    public override string ToString() => $"{(Channels == 1 ? "أحادي / Mono" : "ستيريو / Stereo")} · {Kbps} kbps";
}

public static class AudioFormatOptions
{
    // Both OPAL transports negotiate separate matching stereo/mono operating points.
    public static AudioFormatOption[] For(AudioSelection codec, CallProtocol protocol) => codec switch
    {
        AudioSelection.AacLd => [new(2, 128), new(1, 64)],
        AudioSelection.AacLc => [new(2, 192), new(1, 96)],
        _ => [new(1, 64)]
    };
}
