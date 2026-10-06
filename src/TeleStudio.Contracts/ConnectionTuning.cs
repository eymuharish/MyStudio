using System.Globalization;

namespace TeleStudio.Contracts;

public enum EngineFamily { Cisco, Sony }
public enum AudioSelection { Auto, AacLd, AacLc, G722 }
public enum ConnectionPreset { Direct, External, Weak, VeryWeak, Custom }
public enum ScreenContentMode { Text, Motion }

public sealed record ConnectionTuning
{
    public bool AacEnabled { get; init; }
    // One-time OPAL audio-default migration; later explicit AAC-off/mono choices persist.
    public int OpalAudioDefaultsVersion { get; init; }
    public int OpalVideoDefaultsVersion { get; init; }
    public ConnectionPreset Mode { get; init; } = ConnectionPreset.Direct;
    public AudioSelection AudioCodec { get; init; } = AudioSelection.Auto;
    public int AudioChannels { get; init; } = 1;
    public int AudioKbps { get; init; } = 64;
    public int JitterMinimumMs { get; init; } = 100;
    public int JitterMaximumMs { get; init; } = 300;
    public int JitterInitialMs { get; init; } = 100;
    public int SendBufferMs { get; init; } = 40;
    public int PacketMs { get; init; }
    public int VideoCeilingKbps { get; init; } = 3000;
    public int TotalSendCeilingKbps { get; init; }
    // Conservative media budget, not a Windows traffic shaper: reserve audio and transport headroom.
    public int EffectiveVideoCeilingKbps => TotalSendCeilingKbps == 0 ? VideoCeilingKbps : Math.Min(VideoCeilingKbps, (TotalSendCeilingKbps - 320) * 9 / 10);
    public int ScreenFps { get; init; } = 15;
    public int CameraFps { get; init; } = 15;
    public VideoResolution ScreenResolution { get; init; } = VideoResolution.Fhd;
    public VideoResolution CameraResolution { get; init; } = VideoResolution.Hd;
    public ScreenContentMode ContentMode { get; init; } = ScreenContentMode.Text;
    public bool AdaptiveVideo { get; init; } = true;

    public static ConnectionTuning Preset(ConnectionPreset preset) => preset switch
    {
        ConnectionPreset.External => new() { Mode = preset, JitterMaximumMs = 600, JitterInitialMs = 200, SendBufferMs = 100, VideoCeilingKbps = 1500, ScreenFps = 10 },
        ConnectionPreset.Weak => new() { Mode = preset, JitterMinimumMs = 150, JitterMaximumMs = 600, JitterInitialMs = 200, SendBufferMs = 100, VideoCeilingKbps = 768, ScreenFps = 5, CameraFps = 5 },
        ConnectionPreset.VeryWeak => new() { Mode = preset, JitterMinimumMs = 200, JitterMaximumMs = 600, JitterInitialMs = 250, SendBufferMs = 150, VideoCeilingKbps = 384, ScreenFps = 2, CameraFps = 5 },
        _ => new() { Mode = preset }
    };

    public void Validate(EngineFamily family, CallProtocol protocol)
    {
        if (!Enum.IsDefined(Mode) || !Enum.IsDefined(AudioCodec) || !Enum.IsDefined(ContentMode) ||
            !Enum.IsDefined(ScreenResolution) || !Enum.IsDefined(CameraResolution)) throw new ArgumentException("Invalid tuning selection.");
        if (AudioCodec == AudioSelection.AacLd && family != EngineFamily.Cisco ||
            AudioCodec == AudioSelection.AacLc && family != EngineFamily.Sony) throw new ArgumentException("Codec does not belong to the selected device family.");
        if (AudioChannels is not (1 or 2) || AudioKbps is not (64 or 96 or 128 or 192)) throw new ArgumentException("Invalid audio format.");
        if (AudioCodec == AudioSelection.G722 && (AudioChannels != 1 || AudioKbps != 64)) throw new ArgumentException("G.722 requires mono / 64 kbps.");
        if (AudioCodec == AudioSelection.AacLd && (AudioChannels == 1 ? AudioKbps != 64 : AudioKbps != 128)) throw new ArgumentException("AAC-LD format is not supported.");
        if (AudioCodec == AudioSelection.AacLc && (AudioChannels == 1 ? AudioKbps != 96 : AudioKbps != 192)) throw new ArgumentException("AAC-LC format is not supported.");
        if (JitterMinimumMs < 100 || JitterMaximumMs > 600 || JitterMinimumMs > JitterMaximumMs ||
            JitterInitialMs < JitterMinimumMs || JitterInitialMs > JitterMaximumMs ||
            JitterMinimumMs % 50 != 0 || JitterMaximumMs % 50 != 0 || JitterInitialMs % 50 != 0) throw new ArgumentException("Jitter must be 100–600 ms, in steps of 50.");
        if (SendBufferMs is not (20 or 40 or 60 or 100 or 150 or 200)) throw new ArgumentException("Invalid send buffer.");
        if (PacketMs != 0 && (AudioCodec != AudioSelection.G722 || PacketMs != 20)) throw new ArgumentException("Packet duration must be supported by the selected codec.");
        if (VideoCeilingKbps is < 128 or > 20000 || ScreenFps is not (2 or 5 or 10 or 15 or 24 or 30) || CameraFps is not (5 or 10 or 15 or 24 or 30)) throw new ArgumentException("Invalid video limit.");
        if (TotalSendCeilingKbps != 0 && TotalSendCeilingKbps is < 512 or > 30000) throw new ArgumentException("Total send budget must be 512–30000 kbps, or 0 for no additional budget.");
    }

    public string AudioCommand => FormattableString.Invariant($"audio-profile {AudioCodec switch { AudioSelection.AacLd => "aac-ld", AudioSelection.AacLc => "aac-lc", AudioSelection.G722 => "g722", _ => "auto" }} {AudioChannels} {AudioKbps} {JitterMinimumMs} {JitterMaximumMs} {JitterInitialMs} {SendBufferMs} {PacketMs}");
    public string VideoCommand(bool desktop) => FormattableString.Invariant($"network-video {(desktop ? ScreenFps : CameraFps)} {EffectiveVideoCeilingKbps} {(ContentMode == ScreenContentMode.Text ? "text" : "motion")}");
}

public sealed record SetConnectionTuning(ConnectionTuning Settings) : EngineCommand;
public sealed record AacAvailabilityChanged(bool Available, string Detail) : EngineEvent;
public sealed record AudioProfileApplied(string Codec, int Channels, int BitrateKbps, int JitterMinimumMs, int JitterMaximumMs, int JitterInitialMs, int SendBufferMs, int PacketMs) : EngineEvent;
public sealed record NetworkVideoApplied(int Fps, int BitrateKbps, string Content) : EngineEvent;
public sealed record AudioDirectionState(string Direction, string Codec, int SampleRate, int Channels, int BitrateKbps) : EngineEvent;
public sealed record TransportStatistics(double? LossPercent, double? JitterMs, double? SendQueueMs, double? AudioTxKbps, double? AudioRxKbps, double? VideoTxKbps, double? VideoRxKbps, double? ReceiveLossPercent = null, double? ReceiveJitterMs = null) : EngineEvent;
