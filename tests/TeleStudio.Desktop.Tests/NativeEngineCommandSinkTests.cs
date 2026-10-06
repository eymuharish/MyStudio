using System.Text;
using TeleStudio.Contracts;
using TeleStudio.Desktop;

namespace TeleStudio.Desktop.Tests;

public sealed class NativeEngineCommandSinkTests
{
    [Fact]
    public async Task Opal_video_transport_reaches_support_consumers_only_for_the_active_call()
    {
        var (sink, _, sip) = await CreateStartedAsync();
        await using (sink)
        {
            var events = new List<EngineEvent>();
            sink.EventReceived += (_, value) => events.Add(value);
            await sink.SendAsync(new Dial("10.0.0.20", CallProtocol.Sip, "MyStudio"), CancellationToken.None);
            sip.Emit("call 0: CONFIRMED (OK)");
            sip.EmitJson(OpalVideoDiagnosticsTests.Wire);
            Assert.Single(events.OfType<OpalVideoTransportChanged>());
            sip.Emit("call 0: DISCONNECTED (Normal call clearing)");
            sip.EmitJson(OpalVideoDiagnosticsTests.Wire.Replace("\"sequence\":5", "\"sequence\":6"));
            Assert.Single(events.OfType<OpalVideoTransportChanged>());
        }
    }
    [Theory]
    [InlineData(CallProtocol.H323)]
    [InlineData(CallProtocol.Sip)]
    public async Task Dial_is_one_exact_six_line_transaction_with_normalized_identity(CallProtocol protocol)
    {
        var (sink, h323, sip) = await CreateStartedAsync();
        await using (sink)
        {
            await sink.SendAsync(new Dial(" 10.0.0.20 ", protocol, "  Essa   Studio  "), CancellationToken.None);

            var selected = protocol == CallProtocol.H323 ? h323 : sip;
            Assert.Equal([
                "network-mode direct", "codec auto", "quality auto", "endpoint auto", "identity RXNzYSBTdHVkaW8", "call  10.0.0.20 "
            ], selected.Commands);
        }
    }

    [Theory]
    [InlineData(CallProtocol.H323)]
    [InlineData(CallProtocol.Sip)]
    public async Task External_vpn_mode_is_sent_to_the_selected_engine_before_dial(CallProtocol protocol)
    {
        var (sink, h323, sip) = await CreateStartedAsync();
        await using (sink)
        {
            await sink.SendAsync(new SetNetworkMode(NetworkConnectionMode.ExternalVpn), CancellationToken.None);
            Assert.Equal("network-mode external", Assert.Single(h323.Commands));
            Assert.Equal("network-mode external", Assert.Single(sip.Commands));
            await sink.SendAsync(new Dial("10.0.0.20", protocol, "EssaStudio"), CancellationToken.None);

            var selected = protocol == CallProtocol.H323 ? h323 : sip;
            Assert.Equal("network-mode external", selected.Commands[0]);
            Assert.Equal("call 10.0.0.20", selected.Commands[^1]);
        }
    }

    [Fact]
    public async Task Pending_preferences_write_nothing_and_h323_rejects_h265_without_claiming_call()
    {
        var (sink, h323, sip) = await CreateStartedAsync();
        await using (sink)
        {
            await sink.SendAsync(new SetMediaPreferences(CodecPreference.H265, QualityMode.Motion, EndpointProfile.Generic), CancellationToken.None);
            Assert.Empty(h323.Commands);
            Assert.Empty(sip.Commands);

            await Assert.ThrowsAsync<NotSupportedException>(() => sink.SendAsync(
                new Dial("10.0.0.21", CallProtocol.H323, "EssaStudio"), CancellationToken.None));
            await sink.SendAsync(new Dial("10.0.0.21", CallProtocol.Sip, "EssaStudio"), CancellationToken.None);

            Assert.Equal("network-mode direct", sip.Commands[0]);
            Assert.Equal("codec h265", sip.Commands[1]);
            Assert.Equal("quality motion", sip.Commands[2]);
            Assert.Equal("endpoint generic", sip.Commands[3]);
        }
    }

    [Fact]
    public async Task Failed_dial_line_rolls_back_only_its_claim_and_allows_retry()
    {
        var h323 = new FakeProtocolConsole(CallProtocol.H323) { FailAtWrite = 3 };
        var sip = new FakeProtocolConsole(CallProtocol.Sip);
        await using var sink = new NativeEngineCommandSink(h323, sip);
        await sink.StartAsync(CancellationToken.None);

        await Assert.ThrowsAsync<IOException>(() => sink.SendAsync(
            new Dial("10.0.0.22", CallProtocol.H323, "EssaStudio"), CancellationToken.None));
        h323.FailAtWrite = null;
        await sink.SendAsync(new Dial("10.0.0.22", CallProtocol.Sip, "EssaStudio"), CancellationToken.None);

        Assert.Equal("call 10.0.0.22", sip.Commands[^1]);
    }

    [Fact]
    public async Task Sip_lifecycle_status_on_stderr_confirms_and_ends_the_dialed_call()
    {
        var (sink, _, sip) = await CreateStartedAsync();
        await using (sink)
        {
            var events = new List<EngineEvent>();
            sink.EventReceived += (_, value) => events.Add(value);
            await sink.SendAsync(new Dial("10.0.0.25", CallProtocol.Sip, "EssaStudio"), CancellationToken.None);

            sip.Emit("call 0: CONFIRMED (OK)", isError: true);
            sip.Emit("call 0: DISCONNECTED (local hangup)", isError: true);

            Assert.Collection(events,
                value => Assert.IsType<CallConnected>(value),
                value => Assert.IsType<CallEnded>(value));
        }
    }

    [Fact]
    public async Task Connected_sip_hangup_retires_the_native_engine_when_its_command_loop_is_stuck()
    {
        var (sink, _, sip) = await CreateStartedAsync();
        await using (sink)
        {
            var events = new List<EngineEvent>();
            sink.EventReceived += (_, value) => events.Add(value);
            await sink.SendAsync(new Dial("10.136.51.25", CallProtocol.Sip, "EssaStudio"), CancellationToken.None);
            sip.Emit("call 0: CONFIRMED (OK)");
            sip.Commands.Clear();

            await sink.SendAsync(new HangUp(), CancellationToken.None);

            Assert.Equal(1, sip.AbortCount);
            Assert.Empty(sip.Commands);
            Assert.Single(events, value => value is CallEnded);
            await sink.SendAsync(new Dial("10.136.51.25", CallProtocol.Sip, "EssaStudio"), CancellationToken.None);
            Assert.Equal("call 10.136.51.25", sip.Commands[^1]);
        }
    }

    [Fact]
    public async Task H323_real_legacy_lifecycle_accepts_answer_and_cleans_up_exactly_once()
    {
        var (sink, h323, sip) = await CreateStartedAsync();
        await using (sink)
        {
            var events = new List<EngineEvent>();
            sink.EventReceived += (_, value) => events.Add(value);

            h323.Emit("incoming-call remote=\"EssaStudio\" action=type-answer-or-reject", isError: false);
            await sink.SendAsync(new Answer(), CancellationToken.None);
            h323.Emit("call-established remote=\"EssaStudio\"", isError: false);
            h323.Emit("call-cleared remote=\"EssaStudio\" reason=ended", isError: false);

            Assert.Equal("answer", Assert.Single(h323.Commands));
            Assert.Collection(events,
                value => Assert.Equal(new IncomingCall("EssaStudio", CallProtocol.H323), value),
                value => Assert.Equal(new CallConnected("EssaStudio", CallProtocol.H323, 0, 0, 0), value),
                value => Assert.Equal(new CallEnded("ended"), value));
            Assert.Single(events, value => value is IncomingCall);
            Assert.Single(events, value => value is CallConnected);
            Assert.Single(events, value => value is CallEnded);

            await sink.SendAsync(new Dial("10.0.0.40", CallProtocol.Sip, "EssaStudio"), CancellationToken.None);
            Assert.Equal("call 10.0.0.40", sip.Commands[^1]);
        }
    }

    [Fact]
    public async Task H323_controls_work_from_directional_readiness_before_any_remote_video_surface()
    {
        var (sink, h323, _) = await CreateStartedAsync();
        await using (sink)
        {
            var events = new List<EngineEvent>();
            sink.EventReceived += (_, value) => events.Add(value);
            await sink.SendAsync(new Dial("127.0.0.1:17620", CallProtocol.H323, "EssaStudio"), CancellationToken.None);
            h323.Emit("call-established remote=\"EssaStudio\"");
            h323.EmitJson("{\"schema\":1,\"type\":\"media-ready\",\"sequence\":1,\"protocol\":\"h323\",\"sourceControl\":true,\"muteControl\":true,\"stats\":false,\"remoteSurface\":false,\"audioControl\":true,\"recording\":true}");
            h323.EmitJson("{\"schema\":1,\"type\":\"audio-control-changed\",\"sequence\":2,\"protocol\":\"h323\",\"roomVolume\":100,\"micGain\":100,\"noiseIsolation\":true}");
            h323.EmitJson("{\"schema\":1,\"type\":\"recording-state\",\"sequence\":3,\"protocol\":\"h323\",\"state\":\"idle\",\"micIncluded\":false,\"roomIncluded\":false,\"elapsedMs\":0,\"fileName\":\"\"}");
            h323.Commands.Clear();

            await sink.SendAsync(new SwitchSource(VideoSource.Desktop, "monitor-one"), CancellationToken.None);
            await sink.SendAsync(new SetMuted(true), CancellationToken.None);
            await sink.SendAsync(new SetAudioControls(70, 110, true), CancellationToken.None);
            await sink.SendAsync(new StartLectureRecording("monitor-one", true, false), CancellationToken.None);

            Assert.Equal([
                "source desktop bW9uaXRvci1vbmU", "mute on", "room-volume 70",
                "mic-gain 110", "noise-isolation on",
                "record start bW9uaXRvci1vbmU mic-on room-off"
            ], h323.Commands);
            var readiness = Assert.IsType<MediaCapabilitiesChanged>(events.Single(value => value is MediaCapabilitiesChanged));
            Assert.True(readiness.SourceControl);
            Assert.True(readiness.MuteControl);
            Assert.True(readiness.AudioControl);
            Assert.True(readiness.Recording);
            Assert.False(readiness.RemoteSurface);
            Assert.DoesNotContain(events, value => value is RemoteSurfaceChanged);
        }
    }

    [Fact]
    public async Task Sip_audio_only_readiness_allows_local_commands_without_remote_surface_or_optimistic_confirmation()
    {
        var (sink, _, sip) = await CreateStartedAsync();
        await using (sink)
        {
            var events = new List<EngineEvent>();
            sink.EventReceived += (_, value) => events.Add(value);
            await sink.SendAsync(new Dial("10.136.51.25", CallProtocol.Sip, "EssaStudio"), CancellationToken.None);
            sip.Emit("call 0: CONFIRMED (OK)");
            sip.EmitJson(Json(1, "media-ready", "\"sourceControl\":true,\"muteControl\":true,\"stats\":false,\"remoteSurface\":false,\"audioControl\":true,\"recording\":true"));
            sip.EmitJson(Json(2, "recording-state", "\"state\":\"idle\",\"micIncluded\":false,\"roomIncluded\":false,\"elapsedMs\":0,\"fileName\":\"\""));
            sip.Commands.Clear();

            await sink.SendAsync(new SwitchSource(VideoSource.Desktop, "monitor-primary"), CancellationToken.None);
            await sink.SendAsync(new SetMuted(true), CancellationToken.None);
            await sink.SendAsync(new StartLectureRecording("monitor-primary", true, false), CancellationToken.None);

            Assert.Equal([
                "source desktop bW9uaXRvci1wcmltYXJ5", "mute on",
                "record start bW9uaXRvci1wcmltYXJ5 mic-on room-off"
            ], sip.Commands);
            Assert.Equal(VideoSource.Camera, sink.ConfirmedSource);
            Assert.False(sink.ConfirmedMuted);
            Assert.Null(sink.ConfirmedSharedDesktopId);
            Assert.Equal(RecordingState.Idle, sink.ConfirmedRecordingState.State);
            Assert.False(Assert.IsType<MediaCapabilitiesChanged>(events.Single(value => value is MediaCapabilitiesChanged)).RemoteSurface);
            Assert.DoesNotContain(events, value => value is RemoteSurfaceChanged);
        }
    }

    [Fact]
    public async Task Connected_sink_forwards_confirmed_audio_codec_state_without_remote_video()
    {
        var (sink, _, sip) = await CreateStartedAsync();
        await using (sink)
        {
            var events = new List<EngineEvent>();
            sink.EventReceived += (_, value) => events.Add(value);
            await sink.SendAsync(new Dial("10.136.51.25", CallProtocol.Sip, "EssaStudio"), CancellationToken.None);
            sip.Emit("call 0: CONFIRMED (OK)");
            sip.EmitJson(Json(1, "audio-codec-state",
                "\"requested\":\"aac-ld\",\"negotiated\":\"g722\",\"sampleRate\":16000,\"channels\":1,\"bitrateKbps\":64,\"sendActive\":true,\"receiveActive\":true"));

            Assert.Equal(
                new AudioCodecStateChanged(AudioCodecKind.AacLd, AudioCodecKind.G722, 16000, 1, 64, true, true),
                Assert.Single(events.OfType<AudioCodecStateChanged>()));
            Assert.DoesNotContain(events, value => value is RemoteSurfaceChanged);
        }
    }

    [Fact]
    public async Task Sip_controls_survive_media_readiness_arriving_before_connected_lifecycle()
    {
        var (sink, _, sip) = await CreateStartedAsync();
        await using (sink)
        {
            await sink.SendAsync(new Dial("10.136.101.12", CallProtocol.Sip, "EssaStudio"), CancellationToken.None);
            sip.EmitJson(Json(1, "media-ready", "\"sourceControl\":true,\"muteControl\":true,\"stats\":false,\"remoteSurface\":false,\"audioControl\":false,\"recording\":false"));
            sip.Emit("call 0: CONFIRMED (OK)", isError: true);
            sip.Commands.Clear();

            await sink.SendAsync(new SwitchSource(VideoSource.Desktop, "monitor-primary"), CancellationToken.None);
            await sink.SendAsync(new SetMuted(true), CancellationToken.None);

            Assert.Equal(["source desktop bW9uaXRvci1wcmltYXJ5", "mute on"], sip.Commands);
        }
    }

    [Theory]
    [InlineData(CallProtocol.Sip)]
    [InlineData(CallProtocol.H323)]
    public async Task Full_local_controls_survive_initial_snapshots_arriving_before_connected_lifecycle(CallProtocol protocol)
    {
        var (sink, h323, sip) = await CreateStartedAsync();
        await using (sink)
        {
            var console = protocol == CallProtocol.Sip ? sip : h323;
            var protocolName = protocol == CallProtocol.Sip ? "sip" : "h323";
            var events = new List<EngineEvent>();
            sink.EventReceived += (_, value) => events.Add(value);
            await sink.SendAsync(new Dial("10.136.51.23", protocol, "EssaStudio"), CancellationToken.None);
            console.EmitJson($"{{\"schema\":1,\"type\":\"media-ready\",\"sequence\":1,\"protocol\":\"{protocolName}\",\"sourceControl\":true,\"muteControl\":true,\"stats\":true,\"remoteSurface\":false,\"audioControl\":true,\"recording\":true,\"systemAudioControl\":true,\"roomMixerControl\":true}}");
            console.EmitJson($"{{\"schema\":1,\"type\":\"audio-control-changed\",\"sequence\":2,\"protocol\":\"{protocolName}\",\"roomVolume\":100,\"micGain\":100,\"noiseIsolation\":true}}");
            console.EmitJson($"{{\"schema\":1,\"type\":\"recording-state\",\"sequence\":3,\"protocol\":\"{protocolName}\",\"state\":\"idle\",\"micIncluded\":false,\"roomIncluded\":false,\"elapsedMs\":0,\"fileName\":\"\"}}");
            if (protocol == CallProtocol.Sip)
                console.Emit("call 0: CONFIRMED (OK)", isError: true);
            else
                console.Emit("call-established remote=\"ip$10.136.51.23:1720\"");
            console.Commands.Clear();

            await sink.SendAsync(new SwitchSource(VideoSource.Desktop, "monitor-primary"), CancellationToken.None);
            await sink.SendAsync(new SetMicrophoneMuted(true), CancellationToken.None);
            await sink.SendAsync(new SetSystemAudioEnabled(true), CancellationToken.None);
            await sink.SendAsync(new SetAudioControls(80, 120, true), CancellationToken.None);
            await sink.SendAsync(new SetRoomAudioMuted(true), CancellationToken.None);
            await sink.SendAsync(new SetRoomMixer(180, 3, 4, 1), CancellationToken.None);
            await sink.SendAsync(new StartLectureRecording("monitor-primary", true, true), CancellationToken.None);

            Assert.Collection(events,
                value => Assert.IsType<CallConnected>(value),
                value => Assert.IsType<MediaCapabilitiesChanged>(value),
                value => Assert.IsType<AudioControlsChanged>(value),
                value => Assert.IsType<RecordingStateChanged>(value));
            Assert.Equal([
                "source desktop bW9uaXRvci1wcmltYXJ5", "mic-mute on", "system-audio on",
                "room-volume 80", "mic-gain 120", "noise-isolation on", "room-mute on",
                "room-mixer 180 3 4 1", "record start bW9uaXRvci1wcmltYXJ5 mic-on room-on"
            ], console.Commands);
        }
    }

    [Theory]
    [InlineData(CallProtocol.Sip)]
    [InlineData(CallProtocol.H323)]
    public async Task Initial_snapshots_do_not_escape_after_engine_generation_ends_during_connected_dispatch(CallProtocol protocol)
    {
        var (sink, h323, sip) = await CreateStartedAsync();
        await using (sink)
        {
            var console = protocol == CallProtocol.Sip ? sip : h323;
            var protocolName = protocol == CallProtocol.Sip ? "sip" : "h323";
            var events = new List<EngineEvent>();
            sink.EventReceived += (_, value) =>
            {
                events.Add(value);
                if (value is CallConnected)
                {
                    console.Exit();
                }
            };
            await sink.SendAsync(new Dial("10.136.51.23", protocol, "EssaStudio"), CancellationToken.None);
            console.EmitJson($"{{\"schema\":1,\"type\":\"media-ready\",\"sequence\":1,\"protocol\":\"{protocolName}\",\"sourceControl\":true,\"muteControl\":true,\"stats\":true,\"remoteSurface\":false,\"audioControl\":true,\"recording\":true,\"systemAudioControl\":true,\"roomMixerControl\":true}}");
            console.EmitJson($"{{\"schema\":1,\"type\":\"audio-control-changed\",\"sequence\":2,\"protocol\":\"{protocolName}\",\"roomVolume\":100,\"micGain\":100,\"noiseIsolation\":true}}");
            console.EmitJson($"{{\"schema\":1,\"type\":\"recording-state\",\"sequence\":3,\"protocol\":\"{protocolName}\",\"state\":\"idle\",\"micIncluded\":false,\"roomIncluded\":false,\"elapsedMs\":0,\"fileName\":\"\"}}");

            if (protocol == CallProtocol.Sip)
                console.Emit("call 0: CONFIRMED (OK)", isError: true);
            else
                console.Emit("call-established remote=\"ip$10.136.51.23:1720\"");

            Assert.Collection(events, value => Assert.IsType<CallConnected>(value), value => Assert.IsType<CallFailed>(value));
            var otherProtocol = protocol == CallProtocol.Sip ? CallProtocol.H323 : CallProtocol.Sip;
            await sink.SendAsync(new Dial("10.136.51.25", otherProtocol, "EssaStudio"), CancellationToken.None);
        }
    }

    [Fact]
    public async Task H323_establishment_keeps_mute_and_share_available_if_readiness_event_is_missing()
    {
        var (sink, h323, _) = await CreateStartedAsync();
        await using (sink)
        {
            await sink.SendAsync(new Dial("10.136.203.14", CallProtocol.H323, "EssaStudio"), CancellationToken.None);
            h323.Emit("call-established remote=\"ip$10.136.203.14:1720\"");
            h323.Commands.Clear();

            await sink.SendAsync(new SwitchSource(VideoSource.Desktop, "monitor-primary"), CancellationToken.None);
            await sink.SendAsync(new SetMuted(true), CancellationToken.None);

            Assert.Equal(["source desktop bW9uaXRvci1wcmltYXJ5", "mute on"], h323.Commands);
        }
    }

    [Fact]
    public async Task Audio_transaction_cannot_interleave_with_concurrent_mute()
    {
        var (sink, _, sip) = await CreateConnectedSipAsync();
        await using (sink)
        {
            var firstWriteEntered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            sip.OnWriteAsync = async command =>
            {
                if (command.StartsWith("room-volume", StringComparison.Ordinal))
                {
                    firstWriteEntered.TrySetResult();
                    await release.Task;
                }
            };
            sip.Commands.Clear();

            var audio = sink.SendAsync(new SetAudioControls(80, 120, true), CancellationToken.None);
            await firstWriteEntered.Task;
            var mute = sink.SendAsync(new SetMuted(true), CancellationToken.None);
            release.TrySetResult();
            await Task.WhenAll(audio, mute);

            Assert.Equal(["room-volume 80", "mic-gain 120", "noise-isolation on", "mute on"], sip.Commands);
        }
    }

    [Fact]
    public async Task Native_confirmation_only_updates_state_and_stderr_or_stale_sequence_is_ignored()
    {
        var (sink, _, sip) = await CreateConnectedSipAsync();
        await using (sink)
        {
            var events = new List<EngineEvent>();
            sink.EventReceived += (_, value) => events.Add(value);
            await sink.SendAsync(new SetMuted(true), CancellationToken.None);
            Assert.False(sink.ConfirmedMuted);

            sip.EmitJson(Json(2, "mute-changed", "\"muted\":true"), isError: true);
            sip.EmitJson(Json(2, "mute-changed", "\"muted\":true"));
            sip.EmitJson(Json(2, "mute-changed", "\"muted\":false"));

            Assert.True(sink.ConfirmedMuted);
            Assert.Single(events, value => value is MuteChanged);
        }
    }

    [Fact]
    public async Task Independent_audio_commands_use_exact_wire_text_and_confirm_only_from_current_json_events()
    {
        var (sink, _, sip) = await CreateStartedAsync();
        await using (sink)
        {
            await sink.SendAsync(new Dial("10.0.0.31", CallProtocol.Sip, "EssaStudio"), default);
            sip.Emit("call 0: CONFIRMED (OK)");
            sip.EmitJson(Json(1, "media-ready", "\"sourceControl\":true,\"muteControl\":true,\"stats\":false,\"remoteSurface\":false,\"audioControl\":true,\"recording\":true,\"systemAudioControl\":true,\"roomMixerControl\":true"));
            sip.Commands.Clear();

            await sink.SendAsync(new SetMicrophoneMuted(true), default);
            await sink.SendAsync(new SetSystemAudioEnabled(true), default);
            await sink.SendAsync(new SetRoomAudioMuted(true), default);
            await sink.SendAsync(new SetRoomMixer(300, -12, 0, 12), default);

            Assert.Equal(["mic-mute on", "system-audio on", "room-mute on", "room-mixer 300 -12 0 12"], sip.Commands);
            Assert.False(sink.ConfirmedMicrophoneMuted);
            Assert.False(sink.ConfirmedSystemAudioEnabled);
            Assert.False(sink.ConfirmedRoomAudioMuted);
            Assert.Equal(new RoomMixerChanged(100, 0, 0, 0), sink.ConfirmedRoomMixer);

            sip.EmitJson(Json(2, "microphone-mute-changed", "\"muted\":true"));
            sip.EmitJson(Json(3, "system-audio-changed", "\"enabled\":true"));
            sip.EmitJson(Json(4, "room-audio-mute-changed", "\"muted\":true"));
            sip.EmitJson(Json(5, "room-mixer-changed", "\"masterPercent\":300,\"bassDb\":-12,\"voiceDb\":0,\"trebleDb\":12"));

            Assert.True(sink.ConfirmedMicrophoneMuted);
            Assert.True(sink.ConfirmedMuted);
            Assert.True(sink.ConfirmedSystemAudioEnabled);
            Assert.True(sink.ConfirmedRoomAudioMuted);
            Assert.Equal(new RoomMixerChanged(300, -12, 0, 12), sink.ConfirmedRoomMixer);
        }
    }

    [Fact]
    public async Task Room_noise_command_reaches_native_and_only_confirmation_updates_state()
    {
        var (sink, _, sip) = await CreateStartedAsync();
        await using (sink)
        {
            await sink.SendAsync(new Dial("10.0.0.31",CallProtocol.Sip,"EssaStudio"),default);
            sip.Emit("call 0: CONFIRMED (OK)");
            sip.EmitJson(Json(1,"media-ready","\"sourceControl\":true,\"muteControl\":true,\"stats\":false,\"remoteSurface\":false,\"audioControl\":true,\"recording\":true,\"systemAudioControl\":true,\"roomMixerControl\":true"));
            sip.Commands.Clear();
            await sink.SendAsync(new SetRoomMixer(100,0,0,0,20),default);
            Assert.Equal("room-mixer 100 0 0 0 20",sip.Commands.Single());
            Assert.Equal(0,sink.ConfirmedRoomMixer.NoiseReductionDb);
            sip.EmitJson(Json(2,"room-mixer-changed","\"masterPercent\":100,\"bassDb\":0,\"voiceDb\":0,\"trebleDb\":0,\"noiseReductionDb\":20"));
            Assert.Equal(20,sink.ConfirmedRoomMixer.NoiseReductionDb);
            await Assert.ThrowsAsync<ArgumentOutOfRangeException>(()=>sink.SendAsync(new SetRoomMixer(100,0,0,0,31),default));
        }
    }

    [Fact]
    public async Task Independent_audio_commands_require_their_capability_and_validate_mixer_bounds()
    {
        var (sink, _, sip) = await CreateConnectedSipAsync();
        await using (sink)
        {
            await Assert.ThrowsAsync<NotSupportedException>(() => sink.SendAsync(new SetSystemAudioEnabled(true), default));
            await Assert.ThrowsAsync<NotSupportedException>(() => sink.SendAsync(new SetRoomAudioMuted(true), default));

            sip.EmitJson(Json(2, "media-ready", "\"sourceControl\":true,\"muteControl\":true,\"stats\":true,\"remoteSurface\":false,\"audioControl\":true,\"recording\":true,\"systemAudioControl\":true,\"roomMixerControl\":true"));
            await Assert.ThrowsAsync<ArgumentOutOfRangeException>(() => sink.SendAsync(new SetRoomMixer(301, 0, 0, 0), default));
            await Assert.ThrowsAsync<ArgumentOutOfRangeException>(() => sink.SendAsync(new SetRoomMixer(100, -13, 0, 0), default));
            await Assert.ThrowsAsync<ArgumentOutOfRangeException>(() => sink.SendAsync(new SetRoomMixer(100, 0, 13, 0), default));
            await Assert.ThrowsAsync<ArgumentOutOfRangeException>(() => sink.SendAsync(new SetRoomMixer(100, 0, 0, -13), default));
        }
    }

    [Fact]
    public async Task Desktop_id_is_encoded_once_from_original_utf8_and_confirmed_only_by_native_event()
    {
        var (sink, _, sip) = await CreateConnectedSipAsync();
        await using (sink)
        {
            var id = " monitor-α ";
            var expected = Convert.ToBase64String(Encoding.UTF8.GetBytes(id)).TrimEnd('=').Replace('+', '-').Replace('/', '_');
            await sink.SendAsync(new SwitchSource(VideoSource.Desktop, id), CancellationToken.None);

            Assert.Equal($"source desktop {expected}", sip.Commands[^1]);
            Assert.Null(sink.ConfirmedSharedDesktopId);
            sip.EmitJson(Json(2, "source-changed", "\"source\":\"desktop\""));
            Assert.Equal(id, sink.ConfirmedSharedDesktopId);
        }
    }

    [Fact]
    public async Task Immediate_source_confirmation_during_write_keeps_the_requested_opaque_id()
    {
        var (sink, _, sip) = await CreateConnectedSipAsync();
        await using (sink)
        {
            sip.OnWriteAsync = command =>
            {
                if (command.StartsWith("source desktop", StringComparison.Ordinal))
                {
                    sip.EmitJson(Json(2, "source-changed", "\"source\":\"desktop\""));
                }
                return Task.CompletedTask;
            };

            await sink.SendAsync(new SwitchSource(VideoSource.Desktop, "monitor-race"), CancellationToken.None);

            Assert.Equal("monitor-race", sink.ConfirmedSharedDesktopId);
        }
    }

    [Fact]
    public async Task Recording_requires_readiness_and_quiesced_snapshot_then_stop_is_idempotent()
    {
        var (sink, _, sip) = await CreateConnectedSipAsync();
        await using (sink)
        {
            sip.EmitJson(Json(2, "recording-state", "\"state\":\"idle\",\"micIncluded\":false,\"roomIncluded\":false,\"elapsedMs\":0,\"fileName\":\"\""));
            sip.Commands.Clear();
            await sink.SendAsync(new StartLectureRecording("monitor-one", true, false), CancellationToken.None);
            Assert.Equal("record start bW9uaXRvci1vbmU mic-on room-off", Assert.Single(sip.Commands));

            sip.EmitJson(Json(3, "recording-state", "\"state\":\"starting\",\"micIncluded\":true,\"roomIncluded\":false,\"elapsedMs\":0,\"fileName\":\"\""));
            await sink.SendAsync(new StopLectureRecording(), CancellationToken.None);
            await sink.SendAsync(new StopLectureRecording(), CancellationToken.None);

            Assert.Equal(1, sip.Commands.Count(command => command == "record stop"));
            Assert.Equal(RecordingState.Starting, sink.ConfirmedRecordingState.State);
        }
    }

    [Fact]
    public async Task New_recording_operation_resets_elapsed_after_retained_completed_snapshot()
    {
        var (sink, _, sip) = await CreateConnectedSipAsync();
        await using (sink)
        {
            sip.EmitJson(Json(2, "recording-state", "\"state\":\"completed\",\"micIncluded\":true,\"roomIncluded\":false,\"elapsedMs\":5000,\"fileName\":\"prior.mp4\""));
            await sink.SendAsync(new StartLectureRecording("monitor-one", false, true), CancellationToken.None);
            sip.EmitJson(Json(3, "recording-state", "\"state\":\"starting\",\"micIncluded\":false,\"roomIncluded\":true,\"elapsedMs\":0,\"fileName\":\"\""));

            Assert.Equal(RecordingState.Starting, sink.ConfirmedRecordingState.State);
            Assert.Equal(0, sink.ConfirmedRecordingState.ElapsedMs);
        }
    }

    [Fact]
    public async Task Legacy_detached_finalization_keeps_rejecting_an_unexpected_live_recording_snapshot()
    {
        var (sink, _, sip) = await CreateConnectedSipAsync();
        await using (sink)
        {
            sip.EmitJson(Json(2, "recording-state", "\"state\":\"idle\",\"micIncluded\":false,\"roomIncluded\":false,\"elapsedMs\":0,\"fileName\":\"\""));
            await sink.SendAsync(new StartLectureRecording("monitor-one", true, false), default);
            sip.EmitJson(Json(3, "recording-state", "\"state\":\"starting\",\"micIncluded\":true,\"roomIncluded\":false,\"elapsedMs\":0,\"fileName\":\"\""));
            sip.EmitJson(Json(4, "recording-state", "\"state\":\"recording\",\"micIncluded\":true,\"roomIncluded\":false,\"elapsedMs\":100,\"fileName\":\"\""));
            sip.Emit("call 0: DISCONNECTED (remote hangup)");
            var events = new List<EngineEvent>(); sink.EventReceived += (_, value) => events.Add(value);
            sip.EmitJson(Json(5, "recording-state", "\"state\":\"recording\",\"micIncluded\":true,\"roomIncluded\":false,\"elapsedMs\":110,\"fileName\":\"\""));
            sip.EmitJson(Json(6, "recording-state", "\"state\":\"finalizing\",\"micIncluded\":true,\"roomIncluded\":false,\"elapsedMs\":110,\"fileName\":\"\""));
            sip.EmitJson(Json(7, "recording-state", "\"state\":\"completed\",\"micIncluded\":true,\"roomIncluded\":false,\"elapsedMs\":120,\"fileName\":\"lecture.mp4\""));
            Assert.DoesNotContain(events, value => value is RecordingStateChanged);
            Assert.Null(sink.HistoricalRecordingFileName);
        }
    }

    [Fact]
    public async Task Detached_finalization_can_finish_during_second_call_without_populating_its_live_recording_state()
    {
        var (sink, _, sip) = await CreateConnectedSipAsync();
        await using (sink)
        {
            sip.EmitJson(Json(2, "recording-state", "\"state\":\"idle\",\"micIncluded\":false,\"roomIncluded\":false,\"elapsedMs\":0,\"fileName\":\"\""));
            await sink.SendAsync(new StartLectureRecording("monitor-one", true, false), CancellationToken.None);
            sip.EmitJson(Json(3, "recording-state", "\"state\":\"starting\",\"micIncluded\":true,\"roomIncluded\":false,\"elapsedMs\":0,\"fileName\":\"\""));
            sip.EmitJson(Json(4, "recording-state", "\"state\":\"recording\",\"micIncluded\":true,\"roomIncluded\":false,\"elapsedMs\":100,\"fileName\":\"\""));
            sip.Emit("call 0: DISCONNECTED (remote hangup)");

            await sink.SendAsync(new Dial("10.0.0.31", CallProtocol.Sip, "EssaStudio"), CancellationToken.None);
            sip.Emit("call 1: CONFIRMED (OK)");
            sip.EmitJson(Json(5, "media-ready", "\"sourceControl\":true,\"muteControl\":true,\"stats\":true,\"remoteSurface\":true,\"audioControl\":true,\"recording\":false"));
            var events = new List<EngineEvent>();
            sink.EventReceived += (_, value) => events.Add(value);

            sip.EmitJson(Json(6, "recording-state", "\"state\":\"finalizing\",\"micIncluded\":true,\"roomIncluded\":false,\"elapsedMs\":110,\"fileName\":\"\""));
            sip.EmitJson(Json(7, "recording-state", "\"state\":\"completed\",\"micIncluded\":true,\"roomIncluded\":false,\"elapsedMs\":120,\"fileName\":\"lecture.mp4\""));

            Assert.Equal("lecture.mp4", sink.HistoricalRecordingFileName);
            Assert.Equal(RecordingState.Idle, sink.ConfirmedRecordingState.State);
            Assert.DoesNotContain(events, value => value is RecordingStateChanged);
        }
    }

    [Fact]
    public async Task Active_failed_state_requires_immediately_preceding_error_and_inclusion_must_match_start_request()
    {
        var (sink, _, sip) = await CreateConnectedSipAsync();
        await using (sink)
        {
            sip.EmitJson(Json(2, "recording-state", "\"state\":\"idle\",\"micIncluded\":false,\"roomIncluded\":false,\"elapsedMs\":0,\"fileName\":\"\""));
            await sink.SendAsync(new StartLectureRecording("monitor-one", true, false), CancellationToken.None);
            sip.EmitJson(Json(3, "recording-state", "\"state\":\"starting\",\"micIncluded\":false,\"roomIncluded\":false,\"elapsedMs\":0,\"fileName\":\"\""));
            Assert.Equal(RecordingState.Idle, sink.ConfirmedRecordingState.State);
            sip.EmitJson(Json(4, "recording-state", "\"state\":\"starting\",\"micIncluded\":true,\"roomIncluded\":false,\"elapsedMs\":0,\"fileName\":\"\""));
            Assert.Equal(RecordingState.Starting, sink.ConfirmedRecordingState.State);

            sip.EmitJson(Json(5, "recording-state", "\"state\":\"failed\",\"micIncluded\":true,\"roomIncluded\":false,\"elapsedMs\":0,\"fileName\":\"\""));
            Assert.Equal(RecordingState.Starting, sink.ConfirmedRecordingState.State);
            sip.EmitJson(Json(6, "recording-error", "\"code\":\"sink-failed\",\"detail\":\"safe\",\"recoverablePart\":false,\"fileName\":\"\""));
            sip.EmitJson(Json(7, "recording-state", "\"state\":\"failed\",\"micIncluded\":true,\"roomIncluded\":false,\"elapsedMs\":0,\"fileName\":\"\""));
            Assert.Equal(RecordingState.Starting, sink.ConfirmedRecordingState.State);
        }
    }

    [Fact]
    public async Task Surface_ack_uses_the_original_console_generation_even_after_call_cleanup()
    {
        var (sink, _, sip) = await CreateConnectedSipAsync();
        await using (sink)
        {
            ulong presentationEpoch = 0;
            sink.EventReceived += (_, e) => { if (e is RemoteSurfaceChanged surface) presentationEpoch = surface.Epoch; };
            sip.EmitJson(Json(2, "remote-surface", "\"hostHandle\":\"10\",\"width\":1920,\"height\":1080,\"format\":\"bgra8-unorm\",\"epoch\":\"9\""));
            sip.Emit("call 0: DISCONNECTED (remote hangup)");
            sip.Commands.Clear();

            await sink.SendSurfaceAcknowledgementAsync(SurfaceAcknowledgement.Removed, presentationEpoch, CancellationToken.None);

            Assert.Equal("surface-removed 9", Assert.Single(sip.Commands));
        }
    }

    [Fact]
    public async Task Invalid_lecturer_and_h265_hardware_profile_are_rejected_before_native_output()
    {
        var (sink, h323, _) = await CreateStartedAsync();
        await using (sink)
        {
            await Assert.ThrowsAsync<ArgumentException>(() => sink.SendAsync(
                new Dial("10.0.0.23", CallProtocol.H323, "عيسى"), CancellationToken.None));
            await Assert.ThrowsAsync<NotSupportedException>(() => sink.SendAsync(
                new SetMediaPreferences(CodecPreference.H265, QualityMode.Auto, EndpointProfile.Sx20), CancellationToken.None));
            Assert.Empty(h323.Commands);
        }
    }

    [Fact]
    public void Process_bootstrap_uses_pid_and_fresh_32_byte_canonical_token_only_in_environment()
    {
        var first = ProtocolConsoleProcess.CreateStartInfo(@"C:\native\engine.exe", "--listen");
        var second = ProtocolConsoleProcess.CreateStartInfo(@"C:\native\engine.exe", "--listen");
        var firstToken = first.Environment["ESSASTUDIO_SURFACE_SESSION_TOKEN"]!;

        Assert.Equal(Environment.ProcessId.ToString(), first.Environment["ESSASTUDIO_SURFACE_HOST_PID"]);
        Assert.True(first.Environment.ContainsKey("ESSASTUDIO_SYSTEM_AUDIO_EXCLUDE_PID"));
        Assert.Equal(
            Environment.ProcessId.ToString(System.Globalization.CultureInfo.InvariantCulture),
            first.Environment["ESSASTUDIO_SYSTEM_AUDIO_EXCLUDE_PID"]);
        Assert.NotEqual(firstToken, first.Environment["ESSASTUDIO_SYSTEM_AUDIO_EXCLUDE_PID"]);
        Assert.Equal(43, firstToken.Length);
        Assert.DoesNotContain('=', firstToken);
        Assert.NotEqual(firstToken, second.Environment["ESSASTUDIO_SURFACE_SESSION_TOKEN"]);
        Assert.DoesNotContain(firstToken, first.Arguments, StringComparison.Ordinal);
    }

    [Fact]
    public void Video_bootstrap_is_child_only_complete_and_has_no_capture_enable_flags()
    {
        var before = Environment.GetEnvironmentVariable("ESSASTUDIO_VIDEO_RESOLUTION");
        var first = ProtocolConsoleProcess.CreateStartInfo(@"C:\native\engine.exe", "--listen",
            new SetVideoPreferences(VideoResolution.Fhd, 900));
        var restart = ProtocolConsoleProcess.CreateStartInfo(@"C:\native\engine.exe", "--listen",
            new SetVideoPreferences(VideoResolution.Sd, null));
        Assert.Equal("FHD", first.Environment["ESSASTUDIO_VIDEO_RESOLUTION"]);
        Assert.Equal("900", first.Environment["ESSASTUDIO_VIDEO_BITRATE_KBPS"]);
        Assert.Equal("SD", restart.Environment["ESSASTUDIO_VIDEO_RESOLUTION"]);
        Assert.Equal("auto", restart.Environment["ESSASTUDIO_VIDEO_BITRATE_KBPS"]);
        Assert.Equal(before, Environment.GetEnvironmentVariable("ESSASTUDIO_VIDEO_RESOLUTION"));
        Assert.DoesNotContain(first.Environment.Keys, key => key.Contains("CAMERA_ENABLED") || key.Contains("DESKTOP_ENABLED"));
        Assert.Equal("--listen", first.Arguments);
    }

    [Fact]
    public async Task Saved_video_preferences_reach_both_children_before_start_without_commands()
    {
        var h323 = new FakeProtocolConsole(CallProtocol.H323);
        var sip = new FakeProtocolConsole(CallProtocol.Sip);
        await using var sink = new NativeEngineCommandSink(h323, sip);
        var saved = new SetVideoPreferences(VideoResolution.Hd, 900);
        sink.ConfigureVideoStartup(saved);
        await sink.StartAsync(CancellationToken.None);
        Assert.Equal(saved, h323.PreferencesAtStart);
        Assert.Equal(saved, sip.PreferencesAtStart);
        Assert.Empty(h323.Commands);
        Assert.Empty(sip.Commands);
        h323.Emit("video-startup-preferences-ready-v1", true);
        sip.Emit("video-startup-preferences-ready-v1", true);
        var changed = new SetVideoPreferences(VideoResolution.Sd, null);
        await sink.SendAsync(changed, CancellationToken.None);
        Assert.Equal(changed, h323.StartupPreferences);
        Assert.Equal(changed, sip.StartupPreferences);
        Assert.Equal("video-preferences sd auto", Assert.Single(h323.Commands));
        Assert.Equal("video-preferences sd auto", Assert.Single(sip.Commands));
    }

    [Fact]
    public void Recording_file_actions_use_only_fixed_videos_child_and_never_launch_for_invalid_or_missing_file()
    {
        var boundary = new FakeRecordingFileBoundary(@"C:\Users\tester\Videos");
        var actions = new RecordingFileActions(boundary);

        Assert.False(actions.TryOpenFile(@"..\secret.mp4", out var invalid));
        Assert.Equal("recording-file-invalid", invalid);
        Assert.False(actions.TryOpenFile("lecture.mp4", out var missing));
        Assert.Equal("recording-file-missing", missing);
        Assert.Empty(boundary.OpenedFiles);

        boundary.ExistingFile = @"C:\Users\tester\Videos\EssaStudio\lecture.mp4";
        Assert.True(actions.TryOpenFile("lecture.mp4", out var error), error);
        Assert.Equal(boundary.ExistingFile, Assert.Single(boundary.OpenedFiles));

        Assert.True(actions.TryOpenFolder(out error), error);
        Assert.Equal(@"C:\Users\tester\Videos\EssaStudio", Assert.Single(boundary.OpenedFolders));
    }

    private static async Task<(NativeEngineCommandSink Sink, FakeProtocolConsole H323, FakeProtocolConsole Sip)> CreateStartedAsync()
    {
        var h323 = new FakeProtocolConsole(CallProtocol.H323);
        var sip = new FakeProtocolConsole(CallProtocol.Sip);
        var sink = new NativeEngineCommandSink(h323, sip);
        await sink.StartAsync(CancellationToken.None);
        return (sink, h323, sip);
    }

    [Fact]
    public void Custom_recording_folder_opens_only_safe_basename_and_never_falls_back_when_unavailable()
    {
        var boundary = new FakeRecordingFileBoundary(@"C:\Users\tester\Videos")
        { ExistingFile = @"D:\Lecture Archive\lecture.mp4" };
        var actions = new RecordingFileActions(boundary, () => @"D:\Lecture Archive\");
        foreach (var unsafeName in new[] { @"..\secret.mp4", "../secret.mp4", @"C:\secret.mp4", @"sub\lecture.mp4", "lecture.mp4:payload" })
        {
            Assert.False(actions.TryOpenFile(unsafeName, out var error));
            Assert.Equal("recording-file-invalid", error);
        }
        Assert.Empty(boundary.OpenedFiles);
        Assert.True(actions.TryOpenFile("lecture.mp4", out var openError), openError);
        Assert.Equal(boundary.ExistingFile, Assert.Single(boundary.OpenedFiles));
        Assert.True(actions.TryOpenFolder(out var folderError), folderError);
        Assert.Equal(@"D:\Lecture Archive", Assert.Single(boundary.OpenedFolders));

        var missing = new RecordingFileActions(boundary, () => throw new DirectoryNotFoundException("Selected removable drive is unavailable"));
        Assert.False(missing.TryOpenFolder(out folderError));
        Assert.Equal("recording-folder-unavailable", folderError);
        Assert.False(missing.TryOpenFile("lecture.mp4", out _));
        Assert.Single(boundary.OpenedFiles);
        Assert.Single(boundary.OpenedFolders);
    }

    [Fact]
    public async Task Pause_resume_waits_for_real_ack_and_rejects_unsolicited_or_advancing_paused_time()
    {
        var (sink, _, sip) = await CreateConnectedSipAsync();
        await using (sink)
        {
            string Snapshot(long seq, string state, long time, bool paused = false) => Json(seq, "recording-state", $"\"state\":\"{state}\",\"micIncluded\":true,\"roomIncluded\":false,\"elapsedMs\":{time},\"fileName\":\"\",\"paused\":{paused.ToString().ToLowerInvariant()}");
            sip.EmitJson(Json(2, "recording-state", "\"state\":\"idle\",\"micIncluded\":false,\"roomIncluded\":false,\"elapsedMs\":0,\"fileName\":\"\""));
            await Assert.ThrowsAsync<InvalidOperationException>(() => sink.SendAsync(new SetLectureRecordingPaused(true), CancellationToken.None));
            await sink.SendAsync(new StartLectureRecording("monitor-one", true, false), CancellationToken.None);
            sip.EmitJson(Snapshot(3, "starting", 0)); sip.EmitJson(Snapshot(4, "recording", 1000));
            sip.EmitJson(Snapshot(5, "recording", 1000, true)); Assert.False(sink.ConfirmedRecordingState.Paused);
            await sink.SendAsync(new SetLectureRecordingPaused(true), CancellationToken.None);
            Assert.Equal("record pause", sip.Commands[^1]); Assert.False(sink.ConfirmedRecordingState.Paused);
            await Assert.ThrowsAsync<InvalidOperationException>(() => sink.SendAsync(new SetLectureRecordingPaused(false), CancellationToken.None));
            sip.EmitJson(Snapshot(6, "recording", 1000, true)); Assert.True(sink.ConfirmedRecordingState.Paused);
            sip.EmitJson(Snapshot(7, "recording", 3000, true)); Assert.Equal(1000, sink.ConfirmedRecordingState.ElapsedMs);
            await sink.SendAsync(new SetLectureRecordingPaused(false), CancellationToken.None);
            Assert.Equal("record resume", sip.Commands[^1]); sip.EmitJson(Snapshot(8, "recording", 1000)); Assert.False(sink.ConfirmedRecordingState.Paused);
            sip.EmitJson(Snapshot(9, "recording", 1500)); Assert.Equal(1500, sink.ConfirmedRecordingState.ElapsedMs);
            await sink.SendAsync(new StopLectureRecording(), CancellationToken.None); Assert.Equal("record stop", sip.Commands[^1]);
        }
    }

    [Fact]
    public async Task Rejected_pause_keeps_recording_progress_and_allows_retry()
    {
        var (sink, _, sip) = await CreateConnectedSipAsync();
        await using (sink)
        {
            string Snapshot(long seq, string state, long time, bool paused = false) => Json(seq, "recording-state", $"\"state\":\"{state}\",\"micIncluded\":true,\"roomIncluded\":false,\"elapsedMs\":{time},\"fileName\":\"\",\"paused\":{paused.ToString().ToLowerInvariant()}");
            sip.EmitJson(Json(2, "recording-state", "\"state\":\"idle\",\"micIncluded\":false,\"roomIncluded\":false,\"elapsedMs\":0,\"fileName\":\"\""));
            await sink.SendAsync(new StartLectureRecording("monitor-one", true, false), CancellationToken.None);
            sip.EmitJson(Snapshot(3, "starting", 0)); sip.EmitJson(Snapshot(4, "recording", 1000));
            await sink.SendAsync(new SetLectureRecordingPaused(true), CancellationToken.None);
            sip.EmitJson(Json(5, "recording-error", "\"code\":\"recording-pause-rejected\",\"detail\":\"Not ready\",\"recoverablePart\":false,\"fileName\":\"\""));
            sip.EmitJson(Snapshot(6, "recording", 1200)); Assert.Equal(1200, sink.ConfirmedRecordingState.ElapsedMs);
            await sink.SendAsync(new SetLectureRecordingPaused(true), CancellationToken.None);
            sip.EmitJson(Snapshot(7, "recording", 1200, true)); Assert.True(sink.ConfirmedRecordingState.Paused);
        }
    }

    private static async Task<(NativeEngineCommandSink Sink, FakeProtocolConsole H323, FakeProtocolConsole Sip)> CreateConnectedSipAsync()
    {
        var result = await CreateStartedAsync();
        await result.Sink.SendAsync(new Dial("10.0.0.30", CallProtocol.Sip, "EssaStudio"), CancellationToken.None);
        result.Sip.Emit("call 0: CONFIRMED (OK)");
        result.Sip.EmitJson(Json(1, "media-ready", "\"sourceControl\":true,\"muteControl\":true,\"stats\":true,\"remoteSurface\":true,\"audioControl\":true,\"recording\":true"));
        return result;
    }

    private static string Json(long sequence, string type, string suffix) =>
        $"{{\"schema\":1,\"type\":\"{type}\",\"sequence\":{sequence},\"protocol\":\"sip\",{suffix}}}";

    private sealed class FakeProtocolConsole(CallProtocol protocol) : IProtocolConsole
    {
        private int _writeCount;
        public CallProtocol Protocol { get; } = protocol;
        public long ProcessGeneration { get; private set; }
        public bool Started { get; private set; }
        public SetVideoPreferences? StartupPreferences { get; private set; }
        public SetVideoPreferences? PreferencesAtStart { get; private set; }
        public void ConfigureVideoStartup(SetVideoPreferences preferences) => StartupPreferences = preferences;
        public int? FailAtWrite { get; set; }
        public int AbortCount { get; private set; }
        public Func<string, Task>? OnWriteAsync { get; set; }
        public List<string> Commands { get; } = [];
        public event EventHandler<ProtocolConsoleLine>? LineReceived;
        public event EventHandler<ProtocolConsoleGenerationEnded>? GenerationEnded;

        public Task StartAsync(CancellationToken cancellationToken)
        {
            Started = true;
            PreferencesAtStart = StartupPreferences;
            ProcessGeneration++;
            return Task.CompletedTask;
        }

        public async Task WriteLineAsync(string command, CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var write = Interlocked.Increment(ref _writeCount);
            if (FailAtWrite == write)
            {
                throw new IOException("synthetic-write-failure");
            }
            Commands.Add(command);
            if (OnWriteAsync is not null)
            {
                await OnWriteAsync(command);
            }
        }

        public void Emit(string line, bool isError = false) =>
            LineReceived?.Invoke(this, new ProtocolConsoleLine(line, isError, ProcessGeneration));

        public void EmitJson(string json, bool isError = false) =>
            LineReceived?.Invoke(this, new ProtocolConsoleLine(Encoding.UTF8.GetBytes(json + "\n"), isError, ProcessGeneration));

        public void Exit() => GenerationEnded?.Invoke(this, new ProtocolConsoleGenerationEnded(ProcessGeneration));
        public Task AbortDialAsync(CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            AbortCount++;
            Exit();
            return Task.CompletedTask;
        }
        public ValueTask DisposeAsync() => ValueTask.CompletedTask;
    }

    private sealed class FakeRecordingFileBoundary(string videosFolder) : IRecordingFileBoundary
    {
        public string VideosFolder { get; } = videosFolder;
        public string? ExistingFile { get; set; }
        public List<string> OpenedFiles { get; } = [];
        public List<string> OpenedFolders { get; } = [];
        public string GetVideosFolder() => VideosFolder;
        public bool FileExists(string path) => string.Equals(path, ExistingFile, StringComparison.OrdinalIgnoreCase);
        public void OpenFile(string path) => OpenedFiles.Add(path);
        public void OpenFolder(string path) => OpenedFolders.Add(path);
    }
}
