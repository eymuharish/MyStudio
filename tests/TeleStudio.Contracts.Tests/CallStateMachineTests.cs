using TeleStudio.Contracts;
using System.Text.Json;

namespace TeleStudio.Contracts.Tests;

public sealed class CallStateMachineTests
{
    [Fact]
    public void Managed_media_contract_has_the_frozen_enum_order_and_command_shapes()
    {
        Assert.Equal([CodecPreference.Auto, CodecPreference.H264, CodecPreference.H265], Enum.GetValues<CodecPreference>());
        Assert.Equal([QualityMode.Auto, QualityMode.Presentation, QualityMode.Motion, QualityMode.Maximum], Enum.GetValues<QualityMode>());
        Assert.Equal(
            [RecordingState.Idle, RecordingState.Starting, RecordingState.Recording, RecordingState.Finalizing, RecordingState.Completed, RecordingState.Failed],
            Enum.GetValues<RecordingState>());

        Assert.Equal(new Dial("10.0.0.2", CallProtocol.H323, "Essa Studio"),
            new Dial("10.0.0.2", CallProtocol.H323, "Essa Studio"));
        Assert.Equal(new SwitchSource(VideoSource.Desktop, "monitor-one"),
            new SwitchSource(VideoSource.Desktop, "monitor-one"));
        Assert.IsType<SetMediaPreferences>(new SetMediaPreferences(CodecPreference.H264, QualityMode.Presentation, EndpointProfile.Sx20));
        Assert.IsType<SetAudioControls>(new SetAudioControls(100, 100, true));
        Assert.IsType<SetMicrophoneMuted>(new SetMicrophoneMuted(true));
        Assert.IsType<SetSystemAudioEnabled>(new SetSystemAudioEnabled(true));
        Assert.IsType<SetRoomAudioMuted>(new SetRoomAudioMuted(true));
        Assert.Equal(new SetRoomMixer(300, -12, 0, 12), new SetRoomMixer(300, -12, 0, 12));
        Assert.IsType<StartLectureRecording>(new StartLectureRecording("monitor-one", true, false));
        Assert.IsType<StopLectureRecording>(new StopLectureRecording());
    }

    [Fact]
    public void Independent_audio_polymorphic_discriminators_match_the_frozen_managed_contract()
    {
        var options = new JsonSerializerOptions(JsonSerializerDefaults.Web);
        var commands = new (EngineCommand Value, string Type)[]
        {
            (new SetMicrophoneMuted(true), "set-microphone-muted"),
            (new SetSystemAudioEnabled(true), "set-system-audio-enabled"),
            (new SetRoomAudioMuted(true), "set-room-audio-muted"),
            (new SetRoomMixer(100, 0, 0, 0), "set-room-mixer")
        };
        var events = new (EngineEvent Value, string Type)[]
        {
            (new MicrophoneMuteChanged(true), "microphone-mute-changed"),
            (new SystemAudioChanged(true), "system-audio-changed"),
            (new RoomAudioMuteChanged(true), "room-audio-mute-changed"),
            (new RoomMixerChanged(100, 0, 0, 0), "room-mixer-changed"),
            (new RoomAudioMeterChanged(1, 1, 0, false), "room-audio-meter")
        };

        foreach (var item in commands)
        {
            using var document = JsonDocument.Parse(JsonSerializer.Serialize(item.Value, options));
            Assert.Equal(item.Type, document.RootElement.GetProperty("type").GetString());
            Assert.Equal(item.Value, JsonSerializer.Deserialize<EngineCommand>(document.RootElement.GetRawText(), options));
        }
        foreach (var item in events)
        {
            using var document = JsonDocument.Parse(JsonSerializer.Serialize(item.Value, options));
            Assert.Equal(item.Type, document.RootElement.GetProperty("type").GetString());
            Assert.Equal(item.Value, JsonSerializer.Deserialize<EngineEvent>(document.RootElement.GetRawText(), options));
        }
    }

    [Fact]
    public void Confirmed_independent_audio_events_project_only_their_owned_state()
    {
        var connected = CallSession.Idle.Apply(
            new CallConnected("10.0.0.2", CallProtocol.Sip, 0, 0, 0));
        var ready = connected.Apply(new MediaCapabilitiesChanged(
            true, true, false, false, true, true, true, true));
        var microphone = ready.Apply(new MicrophoneMuteChanged(true));
        var system = microphone.Apply(new SystemAudioChanged(true));
        var roomMute = system.Apply(new RoomAudioMuteChanged(true));
        var mixer = roomMute.Apply(new RoomMixerChanged(300, -12, 0, 12));
        var meter = mixer.Apply(new RoomAudioMeterChanged(32767, 29204, 1234, true));

        Assert.True(ready.SystemAudioControlAvailable);
        Assert.True(ready.RoomMixerControlAvailable);
        Assert.True(microphone.IsMuted);
        Assert.True(system.SystemAudioEnabled);
        Assert.True(roomMute.RoomAudioMuted);
        Assert.Equal(300, mixer.RoomMasterPercent);
        Assert.Equal(-12, mixer.RoomBassDb);
        Assert.Equal(12, mixer.RoomTrebleDb);
        Assert.Equal(32767, meter.RoomPreLimiterPeakQ15);
        Assert.Equal(29204, meter.RoomPostLimiterPeakQ15);
        Assert.Equal(1234, meter.RoomLimiterGainReductionQ15);
        Assert.True(meter.RoomLimiterActive);
    }

    [Fact]
    public void Confirmed_media_events_update_only_their_owned_call_state()
    {
        var connected = CallSession.Idle.Apply(
            new CallConnected("10.0.0.2", CallProtocol.Sip, 1920, 1080, 30));

        var ready = connected.Apply(new MediaCapabilitiesChanged(true, true, true, true, true, true));
        var muted = ready.Apply(new MuteChanged(true));
        var audio = muted.Apply(new AudioControlsChanged(75, 120, true));
        var recording = audio.Apply(new RecordingStateChanged(
            RecordingState.Starting, true, false, 0, string.Empty));

        Assert.True(ready.SourceControlAvailable);
        Assert.True(ready.RecordingAvailable);
        Assert.True(muted.IsMuted);
        Assert.Equal(75, audio.RoomVolume);
        Assert.Equal(RecordingState.Starting, recording.RecordingState);
    }

    [Fact]
    public void Connected_camera_can_switch_to_desktop()
    {
        var session = CallSession.Idle.Apply(
            new CallConnected("10.0.0.2", CallProtocol.H323, 1920, 1080, 30));

        var updated = session.Apply(new LocalSourceChanged(VideoSource.Desktop));

        Assert.Equal(CallState.ConnectedDesktop, updated.State);
        Assert.Equal(VideoSource.Desktop, updated.Source);
    }

    [Fact]
    public void Busy_session_rejects_second_incoming_call()
    {
        var session = CallSession.Idle.Apply(
            new CallConnected("10.0.0.2", CallProtocol.H323, 1280, 720, 30));

        Assert.Throws<InvalidOperationException>(() =>
            session.Apply(new IncomingCall("10.0.0.3", CallProtocol.Sip)));
    }

    [Fact]
    public void Outgoing_call_moves_through_progress_to_connected()
    {
        var calling = CallSession.Idle.Apply(
            new CallProgress("10.0.0.2", CallProtocol.H323, CallState.Calling));
        var negotiating = calling.Apply(
            new CallProgress("10.0.0.2", CallProtocol.H323, CallState.Negotiating));
        var connected = negotiating.Apply(
            new CallConnected("10.0.0.2", CallProtocol.H323, 1920, 1080, 30));

        Assert.Equal(CallState.Calling, calling.State);
        Assert.Equal(CallState.Negotiating, negotiating.State);
        Assert.Equal(CallState.ConnectedCamera, connected.State);
        Assert.Equal(1920, connected.VideoWidth);
    }

    [Fact]
    public void Ending_a_call_returns_to_idle_and_clears_remote_details()
    {
        var connected = CallSession.Idle.Apply(
            new CallConnected("10.0.0.2", CallProtocol.Sip, 1280, 720, 30));

        var ended = connected.Apply(new CallEnding()).Apply(new CallEnded("local-hangup"));

        Assert.Equal(CallSession.Idle, ended);
    }

    [Fact]
    public void Invalid_progress_stage_is_rejected()
    {
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            CallSession.Idle.Apply(
                new CallProgress("10.0.0.2", CallProtocol.H323, CallState.ConnectedCamera)));
    }
}
