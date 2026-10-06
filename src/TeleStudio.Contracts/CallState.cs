namespace TeleStudio.Contracts;

public enum CallState
{
    Idle,
    Incoming,
    Calling,
    Negotiating,
    ConnectedCamera,
    ConnectedDesktop,
    Ending,
    Failed
}

public enum VideoSource
{
    Camera,
    Desktop
}

public sealed record CallSession(
    CallState State,
    string? RemoteAddress,
    CallProtocol? Protocol,
    VideoSource Source,
    int VideoWidth,
    int VideoHeight,
    int FramesPerSecond,
    string? ErrorCode,
    bool SourceControlAvailable = false,
    bool MuteControlAvailable = false,
    bool StatsAvailable = false,
    bool RemoteSurfaceAvailable = false,
    bool AudioControlAvailable = false,
    bool RecordingAvailable = false,
    bool IsMuted = false,
    int RoomVolume = 100,
    int MicrophoneGain = 100,
    bool NoiseIsolation = true,
    RecordingState RecordingState = RecordingState.Idle,
    bool SystemAudioControlAvailable = false,
    bool RoomMixerControlAvailable = false,
    bool SystemAudioEnabled = false,
    bool RoomAudioMuted = false,
    int RoomMasterPercent = 100,
    int RoomBassDb = 0,
    int RoomVoiceDb = 0,
    int RoomTrebleDb = 0,
    int RoomPreLimiterPeakQ15 = 0,
    int RoomPostLimiterPeakQ15 = 0,
    int RoomLimiterGainReductionQ15 = 0,
    bool RoomLimiterActive = false)
{
    public static CallSession Idle { get; } = new(
        CallState.Idle, null, null, VideoSource.Camera, 0, 0, 0, null);

    public CallSession Apply(EngineEvent engineEvent) => engineEvent switch
    {
        IncomingCall incoming when State == CallState.Idle => this with
        {
            State = CallState.Incoming,
            RemoteAddress = incoming.RemoteAddress,
            Protocol = incoming.Protocol,
            ErrorCode = null
        },

        CallProgress progress when progress.Stage is not (CallState.Calling or CallState.Negotiating) =>
            throw new ArgumentOutOfRangeException(nameof(engineEvent), progress.Stage, "Invalid call progress stage."),

        CallProgress progress when State is CallState.Idle or CallState.Calling or CallState.Negotiating => this with
        {
            State = progress.Stage,
            RemoteAddress = progress.RemoteAddress,
            Protocol = progress.Protocol,
            ErrorCode = null
        },

        CallConnected connected when State is CallState.Idle or CallState.Incoming or CallState.Calling or CallState.Negotiating => this with
        {
            State = CallState.ConnectedCamera,
            RemoteAddress = connected.RemoteAddress,
            Protocol = connected.Protocol,
            Source = VideoSource.Camera,
            VideoWidth = connected.VideoWidth,
            VideoHeight = connected.VideoHeight,
            FramesPerSecond = connected.FramesPerSecond,
            ErrorCode = null
        },

        LocalSourceChanged changed when State is CallState.ConnectedCamera or CallState.ConnectedDesktop => this with
        {
            State = changed.Source == VideoSource.Camera
                ? CallState.ConnectedCamera
                : CallState.ConnectedDesktop,
            Source = changed.Source
        },

        MediaCapabilitiesChanged capabilities when IsConnected => this with
        {
            SourceControlAvailable = capabilities.SourceControl,
            MuteControlAvailable = capabilities.MuteControl,
            StatsAvailable = capabilities.Stats,
            RemoteSurfaceAvailable = capabilities.RemoteSurface,
            AudioControlAvailable = capabilities.AudioControl,
            RecordingAvailable = capabilities.Recording,
            SystemAudioControlAvailable = capabilities.SystemAudioControl,
            RoomMixerControlAvailable = capabilities.RoomMixerControl
        },

        MuteChanged changed when IsConnected => this with { IsMuted = changed.IsMuted },
        MicrophoneMuteChanged changed when IsConnected => this with { IsMuted = changed.IsMuted },
        SystemAudioChanged changed when IsConnected => this with { SystemAudioEnabled = changed.Enabled },
        RoomAudioMuteChanged changed when IsConnected => this with { RoomAudioMuted = changed.IsMuted },
        RoomMixerChanged changed when IsConnected => this with
        {
            RoomMasterPercent = changed.MasterPercent,
            RoomBassDb = changed.BassDb,
            RoomVoiceDb = changed.VoiceDb,
            RoomTrebleDb = changed.TrebleDb
        },
        RoomAudioMeterChanged changed when IsConnected => this with
        {
            RoomPreLimiterPeakQ15 = changed.PreLimiterPeakQ15,
            RoomPostLimiterPeakQ15 = changed.PostLimiterPeakQ15,
            RoomLimiterGainReductionQ15 = changed.LimiterGainReductionQ15,
            RoomLimiterActive = changed.LimiterActive
        },

        AudioControlsChanged changed when IsConnected => this with
        {
            RoomVolume = changed.RoomVolume,
            MicrophoneGain = changed.MicrophoneGain,
            NoiseIsolation = changed.NoiseIsolation
        },

        MediaStatsChanged changed when IsConnected => this with
        {
            Source = changed.Source,
            State = changed.Source == VideoSource.Camera ? CallState.ConnectedCamera : CallState.ConnectedDesktop,
            VideoWidth = changed.VideoWidth,
            VideoHeight = changed.VideoHeight,
            FramesPerSecond = changed.FramesPerSecond
        },

        RecordingStateChanged changed when IsConnected => this with { RecordingState = changed.State },

        CallEnding when State is CallState.Incoming or CallState.Calling or CallState.Negotiating
            or CallState.ConnectedCamera or CallState.ConnectedDesktop => this with { State = CallState.Ending },

        CallEnded when State != CallState.Idle => Idle,

        CallFailed failed when State != CallState.Idle => this with
        {
            State = CallState.Failed,
            ErrorCode = failed.ErrorCode
        },

        _ => throw new InvalidOperationException(
            $"Event {engineEvent.GetType().Name} is invalid while call state is {State}.")
    };

    private bool IsConnected => State is CallState.ConnectedCamera or CallState.ConnectedDesktop;
}
