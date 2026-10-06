using System.Text;
using TeleStudio.Contracts;

namespace TeleStudio.Desktop.Tests;

public sealed class SharedOpalConsoleTests
{
    [Fact]
    public async Task Concurrent_disposal_of_one_view_does_not_retire_the_other_view_or_double_dispose_worker()
    {
        var worker = new Worker();
        var shared = new SharedOpalConsole(worker);
        await shared.Sip.StartAsync(default);
        var start = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var tasks = Enumerable.Range(0, 64).Select(_ => Task.Run(async () => { await start.Task; await shared.Sip.DisposeAsync(); })).ToArray();
        start.SetResult(); await Task.WhenAll(tasks);
        Assert.Equal(0, worker.Disposals);
        await shared.H323.WriteLineAsync("network-mode external", default);
        await shared.H323.DisposeAsync();
        Assert.Equal(1, worker.Disposals);
        await Task.WhenAll(shared.Sip.DisposeAsync().AsTask(), shared.H323.DisposeAsync().AsTask());
        Assert.Equal(1, worker.Disposals);
    }
    [Fact]
    public async Task Accepted_call_is_not_released_when_its_original_pipe_write_reports_timeout()
    {
        var worker = new Worker { ThrowAfterCallConfirmation = true };
        await using var sink = new OpalEngineCommandSink(new SharedOpalConsole(worker), EngineFamily.Cisco);
        await sink.StartAsync(default);
        await Assert.ThrowsAsync<TimeoutException>(() => sink.SendAsync(new Dial("192.0.2.22", CallProtocol.Sip, "MyStudio"), default));
        Assert.NotNull(sink.ConnectedCallGeneration);
        await Assert.ThrowsAsync<InvalidOperationException>(() => sink.ActivateProtocolAsync(CallProtocol.H323, default));
        await Assert.ThrowsAsync<InvalidOperationException>(() => sink.SendAsync(new Dial("192.0.2.23", CallProtocol.H323, "MyStudio"), default));
        await sink.SendAsync(new HangUp(), default);
        Assert.Null(sink.ConnectedCallGeneration);
        worker.ThrowAfterCallConfirmation = false;
        await sink.ActivateProtocolAsync(CallProtocol.H323, default);
        await sink.SendAsync(new Dial("192.0.2.23", CallProtocol.H323, "MyStudio"), default);
        Assert.NotNull(sink.ConnectedCallGeneration);
    }
    [Fact]
    public async Task Disposal_cancels_a_blocked_command_before_retiring_shared_worker_and_allows_replacement()
    {
        var worker = new Worker();
        var sink = new OpalEngineCommandSink(new SharedOpalConsole(worker), EngineFamily.Cisco);
        await sink.StartAsync(default);
        worker.BlockCommand = "network-mode external";
        var command = sink.SendAsync(new SetNetworkMode(NetworkConnectionMode.ExternalVpn), default);
        await worker.BlockEntered.Task.WaitAsync(TimeSpan.FromSeconds(2));
        var disposal = sink.DisposeAsync().AsTask();
        try
        {
            await disposal.WaitAsync(TimeSpan.FromSeconds(2));
            await Assert.ThrowsAnyAsync<OperationCanceledException>(() => command);
            Assert.Equal(1, worker.Disposals);
            await Assert.ThrowsAsync<ObjectDisposedException>(() => sink.SendAsync(new SetConnectionTuning(new()), default));
            var nextWorker = new Worker();
            await using var replacement = new OpalEngineCommandSink(new SharedOpalConsole(nextWorker), EngineFamily.Sony);
            await replacement.StartAsync(default);
            Assert.Equal(1, nextWorker.Starts);
        }
        finally { worker.ReleaseBlock.TrySetResult(); try { await command; } catch { } await disposal; }
    }
    [Theory]
    [InlineData(EngineFamily.Cisco, "aac-ld", 128)]
    [InlineData(EngineFamily.Sony, "aac-lc", 192)]
    public async Task Default_dial_prefers_aac_stereo_with_external_buffers_when_plugin_is_ready(EngineFamily family, string codec, int kbps)
    {
        var worker = new Worker();
        await using var sink = new OpalEngineCommandSink(new SharedOpalConsole(worker), family);
        await sink.StartAsync(default);
        worker.Emit("{\"schema\":1,\"type\":\"aac-availability\",\"sequence\":1,\"protocol\":\"sip\",\"available\":true,\"detail\":\"loaded\"}");
        await sink.SendAsync(new Dial("192.0.2.22", CallProtocol.Sip, "MyStudio OPAL"), default);
        Assert.Contains("aac-enabled on", worker.Commands);
        Assert.Contains($"audio-profile {codec} 2 {kbps} 100 600 200 100 0", worker.Commands);
        Assert.Contains("network-mode external", worker.Commands);
        Assert.DoesNotContain("network-mode building", worker.Commands);
    }
    [Theory]
    [InlineData(EngineFamily.Cisco)]
    [InlineData(EngineFamily.Sony)]
    public async Task Global_environment_buffers_override_profile_values_and_native_preflight_confirms_them(EngineFamily family)
    {
        var worker = new Worker();
        await using var sink = new OpalEngineCommandSink(new SharedOpalConsole(worker), family);
        var acknowledgments = new List<AudioProfileApplied>();
        var incoming = new List<IncomingCall>();
        sink.EventReceived += (_, value) => { if (value is AudioProfileApplied applied) acknowledgments.Add(applied); };
        sink.EventReceived += (_, value) => { if (value is IncomingCall call) incoming.Add(call); };
        await sink.StartAsync(default);
        worker.Emit("{\"schema\":1,\"type\":\"aac-availability\",\"sequence\":1,\"protocol\":\"sip\",\"available\":true,\"detail\":\"loaded\"}");
        var endpoints = family == EngineFamily.Cisco ? new[] { EndpointProfile.Sx20, EndpointProfile.Ex90 } : new[] { EndpointProfile.SonyXg77, EndpointProfile.SonyXg80 };
        foreach (var mode in new[] { NetworkConnectionMode.ExternalVpn, NetworkConnectionMode.Building })
        foreach (var protocol in Enum.GetValues<CallProtocol>())
        foreach (var endpoint in endpoints)
        {
            await sink.ActivateProtocolAsync(protocol, default);
            await sink.SendAsync(new SetNetworkMode(mode), default);
            await sink.SendAsync(new SetMediaPreferences(CodecPreference.H264, QualityMode.Auto, endpoint), default);
            var saved = ConnectionTuning.Preset(ConnectionPreset.Weak) with { AacEnabled = true, CameraResolution = VideoResolution.Fhd, VideoCeilingKbps = 4200, CameraFps = 24, ScreenFps = 30 };
            await sink.SendAsync(new SetConnectionTuning(saved), default);
            var normalized = sink.NormalizeTuning(saved, protocol);
            Assert.True(normalized.AacEnabled); Assert.Equal(VideoResolution.Fhd, normalized.CameraResolution);
            Assert.Equal(4200, normalized.VideoCeilingKbps); Assert.Equal(24, normalized.CameraFps); Assert.Equal(30, normalized.ScreenFps);
            var preset = ConnectionTuning.Preset(ConnectionPreset.External);
            Assert.Equal(preset.JitterMinimumMs, normalized.JitterMinimumMs);
            Assert.Equal(preset.JitterMaximumMs, normalized.JitterMaximumMs);
            Assert.Equal(preset.JitterInitialMs, normalized.JitterInitialMs);
            Assert.Equal(preset.SendBufferMs, normalized.SendBufferMs);
            acknowledgments.Clear();
            await sink.SendAsync(new Dial("10.0.0.20", protocol, "MyStudio OPAL"), default);
            Assert.Equal(normalized.AudioCommand, worker.Commands.Last(x => x.StartsWith("audio-profile ")));
            var ack = Assert.Single(acknowledgments);
            Assert.Equal(preset.JitterMaximumMs, ack.JitterMaximumMs);
            Assert.Equal(protocol == CallProtocol.H323 ? preset.JitterMinimumMs : preset.JitterInitialMs, ack.JitterInitialMs);
            Assert.Equal(preset.SendBufferMs, ack.SendBufferMs);
            await sink.SendAsync(new HangUp(), default);
        }
        // An idle global change applies to initial incoming calls immediately, without a further profile save/dial.
        acknowledgments.Clear();
        await sink.SendAsync(new SetNetworkMode(NetworkConnectionMode.ExternalVpn), default);
        var idleAck = Assert.Single(acknowledgments);
        Assert.Equal(600, idleAck.JitterMaximumMs); Assert.Equal(100, idleAck.SendBufferMs);
        worker.Emit("incoming-call remote=\"10.0.0.30\" action=answer-or-reject", true);
        Assert.Single(incoming);
        Assert.Equal(600, idleAck.JitterMaximumMs);
    }

    [Theory]
    [InlineData(CallProtocol.Sip, false)]
    [InlineData(CallProtocol.Sip, true)]
    [InlineData(CallProtocol.H323, false)]
    [InlineData(CallProtocol.H323, true)]
    public async Task Queued_recording_snapshot_after_call_termination_retains_opal_finalization_and_allows_the_next_job(CallProtocol protocol, bool paused)
    {
        var worker = new Worker();
        await using var sink = new OpalEngineCommandSink(new SharedOpalConsole(worker), EngineFamily.Cisco);
        await sink.StartAsync(default);
        await sink.SendAsync(new Dial("10.0.0.20", protocol, "MyStudio OPAL"), default);
        var name = protocol == CallProtocol.Sip ? "sip" : "h323";
        void Ready(int sequence) => worker.Emit($"{{\"schema\":1,\"type\":\"media-ready\",\"sequence\":{sequence},\"protocol\":\"{name}\",\"sourceControl\":true,\"muteControl\":true,\"stats\":true,\"remoteSurface\":false,\"audioControl\":true,\"recording\":true}}");
        void Snapshot(int sequence, string state, int elapsed, bool isPaused = false, string file = "") => worker.Emit(
            $"{{\"schema\":1,\"type\":\"recording-state\",\"sequence\":{sequence},\"protocol\":\"{name}\",\"state\":\"{state}\",\"micIncluded\":{(state == "idle" ? "false" : "true")},\"roomIncluded\":false,\"elapsedMs\":{elapsed},\"fileName\":\"{file}\",\"paused\":{isPaused.ToString().ToLowerInvariant()}}}");
        Ready(100); Snapshot(101, "idle", 0);
        await sink.SendAsync(new StartLectureRecording("monitor-one", true, false), default);
        Snapshot(102, "starting", 0); Snapshot(103, "recording", 1000);
        if (paused) Snapshot(104, "recording", 1300, true);
        worker.Emit(protocol == CallProtocol.Sip ? "call 1: DISCONNECTED (local-hangup)" : "call-cleared remote=\"10.0.0.20\" reason=local-hangup", true);
        var events = new List<EngineEvent>(); sink.EventReceived += (_, value) => events.Add(value);
        Snapshot(105, "recording", paused ? 1300 : 1100, paused);
        Snapshot(106, "finalizing", 1300); Snapshot(107, "completed", 1300, file: "paused-lecture.mp4");
        Assert.Equal(new[] { RecordingState.Finalizing, RecordingState.Completed }, events.OfType<RecordingStateChanged>().Select(x => x.State));
        Assert.Equal("paused-lecture.mp4", sink.HistoricalRecordingFileName);
        await sink.SendAsync(new Dial("10.0.0.21", protocol, "MyStudio OPAL"), default);
        Ready(200); Snapshot(201, "idle", 0);
        await sink.SendAsync(new StartLectureRecording("monitor-one", true, false), default);
        Assert.Equal("record start bW9uaXRvci1vbmU mic-on room-off 1920 1080", worker.Commands.Last());
    }

    [Theory]
    [InlineData(CallProtocol.Sip)]
    [InlineData(CallProtocol.H323)]
    public async Task Automatic_opal_share_pause_is_confirmed_but_resume_requires_an_explicit_command(CallProtocol protocol)
    {
        var worker = new Worker();
        await using var sink = new OpalEngineCommandSink(new SharedOpalConsole(worker), EngineFamily.Cisco);
        await sink.StartAsync(default);
        await sink.SendAsync(new Dial("10.0.0.20", protocol, "MyStudio OPAL"), default);
        var name = protocol == CallProtocol.Sip ? "sip" : "h323";
        worker.Emit($"{{\"schema\":1,\"type\":\"media-ready\",\"sequence\":100,\"protocol\":\"{name}\",\"sourceControl\":true,\"muteControl\":true,\"stats\":true,\"remoteSurface\":false,\"audioControl\":true,\"recording\":true}}");
        string Snapshot(int sequence, string state, int elapsed, bool paused = false) =>
            $"{{\"schema\":1,\"type\":\"recording-state\",\"sequence\":{sequence},\"protocol\":\"{name}\",\"state\":\"{state}\",\"micIncluded\":true,\"roomIncluded\":false,\"elapsedMs\":{elapsed},\"fileName\":\"\",\"paused\":{paused.ToString().ToLowerInvariant()}}}";
        worker.Emit($"{{\"schema\":1,\"type\":\"recording-state\",\"sequence\":101,\"protocol\":\"{name}\",\"state\":\"idle\",\"micIncluded\":false,\"roomIncluded\":false,\"elapsedMs\":0,\"fileName\":\"\",\"paused\":false}}");
        await sink.SendAsync(new StartLectureRecording("monitor-one", true, false), default);
        worker.Emit(Snapshot(102, "starting", 0)); worker.Emit(Snapshot(103, "recording", 1000));
        Assert.Equal(RecordingState.Recording, sink.ConfirmedRecordingState.State);
        worker.Emit(Snapshot(104, "recording", 1300, true));
        Assert.True(sink.ConfirmedRecordingState.Paused);
        Assert.Equal(RecordingState.Recording, sink.ConfirmedRecordingState.State);
        worker.Emit(Snapshot(105, "recording", 1500, true));
        Assert.Equal(1300, sink.ConfirmedRecordingState.ElapsedMs);
        worker.Emit(Snapshot(106, "recording", 1300));
        Assert.True(sink.ConfirmedRecordingState.Paused);
        await sink.SendAsync(new SetLectureRecordingPaused(false), default);
        Assert.Equal("record resume", worker.Commands.Last());
        worker.Emit(Snapshot(107, "recording", 1300));
        Assert.False(sink.ConfirmedRecordingState.Paused);
    }

    [Theory]
    [InlineData(EngineFamily.Cisco, CallProtocol.Sip)]
    [InlineData(EngineFamily.Cisco, CallProtocol.H323)]
    [InlineData(EngineFamily.Sony, CallProtocol.Sip)]
    [InlineData(EngineFamily.Sony, CallProtocol.H323)]
    public async Task Every_opal_record_start_enforces_1080p_independently_of_live_call_resolution(EngineFamily family, CallProtocol protocol)
    {
        var worker = new Worker();
        await using var sink = new OpalEngineCommandSink(new SharedOpalConsole(worker), family);
        await sink.StartAsync(default);
        await sink.SendAsync(new SetVideoPreferences(VideoResolution.Hd, 1600), default);
        await sink.SendAsync(new Dial("10.0.0.20", protocol, "MyStudio OPAL"), default);
        var name = protocol == CallProtocol.Sip ? "sip" : "h323";
        worker.Emit($"{{\"schema\":1,\"type\":\"media-ready\",\"sequence\":100,\"protocol\":\"{name}\",\"sourceControl\":true,\"muteControl\":true,\"stats\":true,\"remoteSurface\":false,\"audioControl\":true,\"recording\":true,\"systemAudioControl\":false,\"roomMixerControl\":false,\"videoControl\":true}}");
        worker.Emit($"{{\"schema\":1,\"type\":\"recording-state\",\"sequence\":101,\"protocol\":\"{name}\",\"state\":\"idle\",\"micIncluded\":false,\"roomIncluded\":false,\"elapsedMs\":0,\"fileName\":\"\",\"paused\":false}}");
        await sink.SendAsync(new StartLectureRecording("monitor-one", true, false), default);
        Assert.Equal("record start bW9uaXRvci1vbmU mic-on room-off 1920 1080", worker.Commands.Last());
    }

    [Theory]
    [InlineData(CallProtocol.Sip, EndpointProfile.Sx20)]
    [InlineData(CallProtocol.H323, EndpointProfile.Ex90)]
    public async Task Fresh_opal_cisco_dial_applies_1080p_and_4000_kbps_before_call_without_starting_camera(CallProtocol protocol, EndpointProfile endpoint)
    {
        var worker = new Worker(); var shared = new SharedOpalConsole(worker);
        await using var sink = new OpalEngineCommandSink(shared, EngineFamily.Cisco);
        await sink.StartAsync(default);
        await sink.SendAsync(new SetMediaPreferences(CodecPreference.H264, QualityMode.Auto, endpoint), default);
        await sink.SendAsync(new Dial("10.0.0.20", protocol, "MyStudio OPAL"), default);
        Assert.Contains("video-preferences fhd 4000", worker.Commands);
        Assert.Contains("network-video 30 4000 text", worker.Commands);
        Assert.True(worker.Commands.IndexOf("video-preferences fhd 4000") < worker.Commands.IndexOf("call 10.0.0.20"));
        Assert.DoesNotContain(worker.Commands, x => x.StartsWith("camera on") || x == "source camera");
    }

    [Fact]
    public async Task Explicit_720p_selection_overrides_the_cisco_default_before_dial()
    {
        var worker = new Worker(); var shared = new SharedOpalConsole(worker);
        await using var sink = new OpalEngineCommandSink(shared, EngineFamily.Cisco);
        await sink.StartAsync(default);
        await sink.SendAsync(new SetVideoPreferences(VideoResolution.Hd, 1600), default);
        await sink.SendAsync(new Dial("10.0.0.20", CallProtocol.Sip, "MyStudio OPAL"), default);
        Assert.True(worker.Commands.LastIndexOf("video-preferences hd 1600") < worker.Commands.IndexOf("call 10.0.0.20"));
        Assert.Equal("video-preferences hd 1600", worker.Commands.Last(x => x.StartsWith("video-preferences ")));
    }

    [Theory]
    [InlineData(CallProtocol.Sip)]
    [InlineData(CallProtocol.H323)]
    public async Task Setup_failure_is_visible_and_releases_call_without_stopping_either_listener(CallProtocol protocol)
    {
        var worker = new Worker { FailNextCall = true };
        await using var sink = new OpalEngineCommandSink(new SharedOpalConsole(worker), EngineFamily.Cisco);
        var events = new List<EngineEvent>(); sink.EventReceived += (_, value) => events.Add(value);
        await sink.StartAsync(default);
        await sink.SendAsync(new Dial("10.0.0.20", protocol, "MyStudio OPAL"), default);
        Assert.Single(events.OfType<CallFailed>());
        Assert.DoesNotContain(events, value => value is CallEnded);
        Assert.Equal(0, worker.Disposals);
        await sink.SendAsync(new Dial("10.0.0.21", protocol, "MyStudio OPAL"), default);
        Assert.Single(events.OfType<CallConnected>());
        Assert.Equal(1, worker.Starts);
    }

    [Theory]
    [InlineData(43001)]
    [InlineData(65138)]
    public void Startup_rejects_rtp_ranges_that_native_cannot_bind(int rtpPort)
    {
        Assert.Throws<ArgumentOutOfRangeException>(() => OpalEngineCommandSink.BuildOpalArguments(5140, 1760, rtpPort, null));
    }

    [Fact]
    public void Highest_rtp_base_reserves_the_full_bfcp_tcp_port_range()
    {
        Assert.Equal("--sip-port 5140 --h323-port 1760 --rtp-port 65136",
            OpalEngineCommandSink.BuildOpalArguments(5140, 1760, 65136, null));
        var error = Assert.Throws<ArgumentOutOfRangeException>(() => OpalEngineCommandSink.BuildOpalArguments(5140, 1760, 65138, null));
        Assert.Contains("65136", error.Message);
    }

    [Theory]
    [InlineData(EngineFamily.Cisco, CallProtocol.Sip, "endpoint sx20", "audio-profile aac-ld 1 64 100 600 200 100 0")]
    [InlineData(EngineFamily.Sony, CallProtocol.H323, "endpoint pcs-xg77", "audio-profile aac-lc 1 96 100 600 200 100 0")]
    public async Task Idle_tuning_applies_family_and_confirmed_media_settings_before_first_incoming_call(EngineFamily family, CallProtocol protocol, string endpoint, string audio)
    {
        var worker = new Worker(); var shared = new SharedOpalConsole(worker);
        await using var sink = new OpalEngineCommandSink(shared, family);
        await sink.StartAsync(default);
        await sink.ActivateProtocolAsync(protocol, default);
        worker.Emit("{\"schema\":1,\"type\":\"aac-availability\",\"sequence\":1,\"protocol\":\"sip\",\"available\":true,\"detail\":\"loaded\"}");
        await sink.SendAsync(new SetNetworkMode(NetworkConnectionMode.ExternalVpn), default);
        sink.ConfigureAudioDevices("selected microphone", "selected speaker");
        await sink.SendAsync(new SetConnectionTuning(ConnectionTuning.Preset(ConnectionPreset.External) with { AacEnabled = true }), default);
        Assert.Contains(endpoint, worker.Commands);
        Assert.Contains(audio, worker.Commands);
        Assert.Contains("network-video 15 1500 text", worker.Commands);
        Assert.Contains("audio-devices c2VsZWN0ZWQgbWljcm9waG9uZQ c2VsZWN0ZWQgc3BlYWtlcg", worker.Commands);
        Assert.True(worker.Commands.IndexOf(endpoint) < worker.Commands.IndexOf("aac-enabled on"));
        Assert.True(worker.Commands.IndexOf("aac-enabled on") < worker.Commands.IndexOf(audio));
        Assert.DoesNotContain(worker.Commands, command => command.StartsWith("call "));
    }

    [Fact]
    public async Task Idle_tuning_preserves_an_explicit_selected_endpoint()
    {
        var worker = new Worker();
        await using var sink = new OpalEngineCommandSink(new SharedOpalConsole(worker), EngineFamily.Cisco);
        await sink.StartAsync(default);
        await sink.SendAsync(new SetMediaPreferences(CodecPreference.H264, QualityMode.Auto, EndpointProfile.Ex90), default);
        await sink.SendAsync(new SetConnectionTuning(new()), default);
        Assert.Contains("endpoint ex90", worker.Commands);
    }

    [Fact]
    public async Task Idle_tuning_rejection_cannot_mark_initial_incoming_configuration_ready()
    {
        var worker = new Worker { RejectAudioProfiles = true };
        await using var sink = new OpalEngineCommandSink(new SharedOpalConsole(worker), EngineFamily.Cisco);
        await sink.StartAsync(default);
        var error = await Assert.ThrowsAsync<InvalidOperationException>(() => sink.SendAsync(new SetConnectionTuning(new()), default));
        Assert.Equal("Profile could not be applied", error.Message);
        Assert.DoesNotContain(worker.Commands, command => command.StartsWith("network-video "));
    }

    [Fact]
    public async Task Idle_tuning_waits_for_actual_native_confirmation()
    {
        var worker = new Worker { SkipAudioAcknowledgment = true };
        await using var sink = new OpalEngineCommandSink(new SharedOpalConsole(worker), EngineFamily.Cisco);
        await sink.StartAsync(default);
        using var deadline = new CancellationTokenSource(TimeSpan.FromMilliseconds(150));
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => sink.SendAsync(new SetConnectionTuning(new()), deadline.Token));
        Assert.DoesNotContain(worker.Commands, command => command.StartsWith("network-video "));
    }

    [Theory]
    [InlineData("surface-released 17")]
    [InlineData("surface-removed 17")]
    [InlineData("local-surface-released 17")]
    [InlineData("local-surface-removed 17")]
    public async Task Old_surface_release_does_not_change_the_new_calls_protocol(string acknowledgement)
    {
        var worker = new Worker(); var shared = new SharedOpalConsole(worker);
        await shared.Sip.StartAsync(default);
        worker.Emit("incoming-call remote=\"10.0.0.4\" action=answer-or-reject", error: true);
        try
        {
            await shared.Sip.WriteLineAsync(acknowledgement, default);
            await shared.H323.WriteLineAsync("answer", default);
            Assert.Equal(new[] { acknowledgement, "answer" }, worker.Commands);
        }
        finally { await shared.Sip.DisposeAsync(); await shared.H323.DisposeAsync(); }
    }

    [Fact]
    public async Task Old_recording_completion_retains_its_origin_during_a_new_other_protocol_call()
    {
        var worker = new Worker(); var shared = new SharedOpalConsole(worker);
        await shared.Sip.StartAsync(default);
        worker.Emit("incoming-call remote=\"10.0.0.4\" action=answer-or-reject", error: true);
        var sipEvents = new List<ProtocolConsoleLine>(); var h323Events = new List<ProtocolConsoleLine>();
        shared.Sip.LineReceived += (_, line) => sipEvents.Add(line); shared.H323.LineReceived += (_, line) => h323Events.Add(line);
        worker.Emit("{\"schema\":1,\"type\":\"recording-state\",\"sequence\":21,\"protocol\":\"sip\",\"state\":\"completed\",\"micIncluded\":true,\"roomIncluded\":true,\"elapsedMs\":5000,\"fileName\":\"lecture.mp4\",\"paused\":false}");
        Assert.Single(sipEvents); Assert.Empty(h323Events);
        await shared.Sip.DisposeAsync(); await shared.H323.DisposeAsync();
    }

    [Theory]
    [InlineData(EngineFamily.Cisco, CallProtocol.Sip, 2, AudioSelection.AacLd, 2, 128)]
    [InlineData(EngineFamily.Cisco, CallProtocol.H323, 2, AudioSelection.AacLd, 2, 128)]
    [InlineData(EngineFamily.Sony, CallProtocol.Sip, 1, AudioSelection.AacLc, 1, 96)]
    [InlineData(EngineFamily.Sony, CallProtocol.H323, 1, AudioSelection.AacLc, 1, 96)]
    public async Task Aac_normalization_limits_formats_to_supported_device_profiles(EngineFamily family, CallProtocol protocol, int desiredChannels, AudioSelection codec, int channels, int kbps)
    {
        await using var sink = new OpalEngineCommandSink(new SharedOpalConsole(new Worker()), family);
        var tuning = sink.NormalizeTuning(new() { AacEnabled = true, AudioChannels = desiredChannels, AudioKbps = 64, PacketMs = 20 }, protocol);
        Assert.Equal(codec, tuning.AudioCodec); Assert.Equal(channels, tuning.AudioChannels); Assert.Equal(kbps, tuning.AudioKbps);
        Assert.Equal(0, tuning.PacketMs); tuning.Validate(family, protocol);
    }

    [Fact]
    public void Relocated_opal_resolution_never_falls_back_to_legacy_engines()
    {
        var root = Directory.CreateTempSubdirectory("OPAL relocation ");
        try
        {
            var legacy = Path.Combine(root.FullName, "Engines", "Cisco", "NativeEngines"); Directory.CreateDirectory(legacy);
            File.WriteAllText(Path.Combine(legacy, "sip_direct_console.exe"), "legacy");
            Assert.Throws<FileNotFoundException>(() => OpalEngineCommandSink.ResolveOpalDirectory(root.FullName));
            var opal = Path.Combine(root.FullName, "Engines", "Opal", "NativeEngines"); Directory.CreateDirectory(opal);
            File.WriteAllText(Path.Combine(opal, "opal_console.exe"), "opal");
            Assert.Equal(opal, OpalEngineCommandSink.ResolveOpalDirectory(root.FullName));
        }
        finally { root.Delete(true); }
    }

    [Fact]
    public async Task Selected_audio_devices_are_sent_before_the_next_call_as_opaque_tokens()
    {
        var worker = new Worker(); var shared = new SharedOpalConsole(worker);
        await using var sink = new OpalEngineCommandSink(shared, EngineFamily.Cisco);
        await sink.StartAsync(default);
        sink.ConfigureAudioDevices("microphone device", null);
        await sink.SendAsync(new Dial("10.0.0.20", CallProtocol.Sip, "MyStudio"), default);
        Assert.Contains("audio-devices bWljcm9waG9uZSBkZXZpY2U default", worker.Commands);
        Assert.True(worker.Commands.IndexOf("audio-devices bWljcm9waG9uZSBkZXZpY2U default") < worker.Commands.IndexOf("call 10.0.0.20"));
    }

    [Fact]
    public async Task Two_protocol_views_start_one_worker_and_keep_it_until_both_views_are_disposed()
    {
        var worker = new Worker();
        var shared = new SharedOpalConsole(worker);
        await Task.WhenAll(shared.Sip.StartAsync(default), shared.H323.StartAsync(default));
        Assert.Equal(1, worker.Starts);
        await shared.Sip.DisposeAsync();
        Assert.Equal(0, worker.Disposals);
        await shared.H323.WriteLineAsync("network-mode direct", default);
        Assert.Equal(new[] { "protocol h323", "network-mode direct" }, worker.Commands);
        await shared.H323.DisposeAsync();
        Assert.Equal(1, worker.Disposals);
    }

    [Fact]
    public async Task Json_is_routed_by_actual_protocol_and_global_sequence_cannot_replay_on_other_view()
    {
        var worker = new Worker();
        var shared = new SharedOpalConsole(worker);
        await shared.Sip.StartAsync(default);
        var sip = new List<ProtocolConsoleLine>(); var h323 = new List<ProtocolConsoleLine>();
        shared.Sip.LineReceived += (_, line) => sip.Add(line);
        shared.H323.LineReceived += (_, line) => h323.Add(line);
        worker.Emit("{\"schema\":1,\"type\":\"aac-availability\",\"sequence\":5,\"protocol\":\"h323\",\"available\":true,\"detail\":\"loaded\"}");
        worker.Emit("{\"schema\":1,\"type\":\"aac-availability\",\"sequence\":5,\"protocol\":\"sip\",\"available\":true,\"detail\":\"duplicate\"}");
        Assert.Empty(sip); Assert.Single(h323);
        await shared.Sip.DisposeAsync(); await shared.H323.DisposeAsync();
    }

    [Fact]
    public async Task Incoming_h323_call_keeps_sip_commands_from_switching_its_protocol()
    {
        var worker = new Worker(); var shared = new SharedOpalConsole(worker);
        await shared.Sip.StartAsync(default);
        worker.Emit("incoming-call remote=\"10.0.0.4\" action=answer-or-reject", error: true);
        await Assert.ThrowsAsync<InvalidOperationException>(() => shared.Sip.WriteLineAsync("aac-enabled off", default));
        await shared.H323.WriteLineAsync("answer", default);
        Assert.Equal(new[] { "answer" }, worker.Commands);
        worker.Emit("call-cleared remote=\"10.0.0.4\" reason=local-hangup", error: true);
        await shared.Sip.WriteLineAsync("aac-enabled off", default);
        Assert.Equal("protocol sip", worker.Commands[^2]);
        await shared.Sip.DisposeAsync(); await shared.H323.DisposeAsync();
    }

    [Fact]
    public async Task Opal_dial_falls_back_to_g722_when_plugin_unavailable_and_keeps_both_listeners()
    {
        var worker = new Worker(); var shared = new SharedOpalConsole(worker);
        await using var sink = new OpalEngineCommandSink(shared, EngineFamily.Cisco);
        await sink.StartAsync(default);
        await sink.SendAsync(new Dial("10.0.0.20", CallProtocol.Sip, "MyStudio OPAL"), default);
        Assert.Contains("aac-enabled off", worker.Commands);
        Assert.Contains("audio-profile g722 1 64 100 600 200 100 0", worker.Commands);
        await sink.SendAsync(new HangUp(), default);
        Assert.Equal(0, worker.Disposals);
        await sink.SendAsync(new Dial("10.0.0.21", CallProtocol.H323, "MyStudio OPAL"), default);
        Assert.Equal("call 10.0.0.21", worker.Commands[^1]);
    }

    [Fact]
    public async Task Aac_requires_plugin_and_cannot_change_during_an_active_call()
    {
        var worker = new Worker(); var shared = new SharedOpalConsole(worker);
        await using var sink = new OpalEngineCommandSink(shared, EngineFamily.Cisco);
        await sink.StartAsync(default);
        await Assert.ThrowsAsync<InvalidOperationException>(() => sink.SendAsync(new SetConnectionTuning(new() { AacEnabled = true }), default));
        worker.Emit("{\"schema\":1,\"type\":\"aac-availability\",\"sequence\":1,\"protocol\":\"sip\",\"available\":true,\"detail\":\"loaded\"}");
        await sink.SendAsync(new SetConnectionTuning(new() { AacEnabled = true }), default);
        await sink.SendAsync(new Dial("10.0.0.20", CallProtocol.Sip, "MyStudio"), default);
        Assert.Contains("aac-enabled on", worker.Commands);
        Assert.Contains("audio-profile aac-ld 1 64 100 600 200 100 0", worker.Commands);
        await Assert.ThrowsAsync<InvalidOperationException>(() => sink.SendAsync(new SetConnectionTuning(new() { AacEnabled = false }), default));
    }

    [Fact]
    public async Task Pending_opal_hangup_waits_past_the_old_cleanup_limits_for_native_terminal()
    {
        var worker = new Worker { DeferCallConfirmation = true, DeferHangupConfirmation = true };
        await using var sink = new OpalEngineCommandSink(new SharedOpalConsole(worker), EngineFamily.Cisco);
        var events = new List<EngineEvent>();
        sink.EventReceived += (_, value) => events.Add(value);
        await sink.StartAsync(default);
        await sink.SendAsync(new Dial("10.0.0.20", CallProtocol.Sip, "MyStudio"), default);
        var hangup = sink.SendAsync(new HangUp(), default);
        await Task.Delay(TimeSpan.FromSeconds(6));
        Assert.False(hangup.IsCompleted);
        Assert.DoesNotContain(events, value => value is CallEnded or CallFailed);
        worker.Emit("call 1: DISCONNECTED (local-hangup)", true);
        await hangup.WaitAsync(TimeSpan.FromSeconds(2));
        Assert.Single(events.OfType<CallEnded>());
        worker.DeferCallConfirmation = false;
        await sink.SendAsync(new Dial("10.0.0.21", CallProtocol.H323, "MyStudio"), default);
        Assert.Equal("call 10.0.0.21", worker.Commands[^1]);
    }

    [Fact]
    public async Task Cancelled_pending_opal_hangup_preserves_busy_state_until_late_native_terminal()
    {
        var worker = new Worker { DeferCallConfirmation = true, DeferHangupConfirmation = true };
        await using var sink = new OpalEngineCommandSink(new SharedOpalConsole(worker), EngineFamily.Cisco);
        var events = new List<EngineEvent>();
        sink.EventReceived += (_, value) => events.Add(value);
        await sink.StartAsync(default);
        await sink.SendAsync(new Dial("10.0.0.20", CallProtocol.Sip, "MyStudio"), default);
        using var cancellation = new CancellationTokenSource();
        var hangup = sink.SendAsync(new HangUp(), cancellation.Token);
        cancellation.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => hangup.WaitAsync(TimeSpan.FromSeconds(1)));
        Assert.DoesNotContain(events, value => value is CallEnded or CallFailed);
        await Assert.ThrowsAsync<InvalidOperationException>(() => sink.SendAsync(new Dial("10.0.0.21", CallProtocol.H323, "MyStudio"), default));
        worker.Emit("call 1: DISCONNECTED (local-hangup)", true);
        Assert.Single(events.OfType<CallEnded>());
        worker.DeferCallConfirmation = false;
        await sink.SendAsync(new Dial("10.0.0.21", CallProtocol.H323, "MyStudio"), default);
        Assert.Equal("call 10.0.0.21", worker.Commands[^1]);
    }

    [Fact]
    public async Task Timed_out_pending_opal_hangup_does_not_release_the_live_call()
    {
        var worker = new Worker { DeferCallConfirmation = true, DeferHangupConfirmation = true };
        var shared = new SharedOpalConsole(worker, TimeSpan.FromMilliseconds(100));
        await using var sink = new OpalEngineCommandSink(shared, EngineFamily.Cisco);
        var events = new List<EngineEvent>();
        sink.EventReceived += (_, value) => events.Add(value);
        await sink.StartAsync(default);
        await sink.SendAsync(new Dial("10.0.0.20", CallProtocol.Sip, "MyStudio"), default);
        await Assert.ThrowsAsync<TimeoutException>(() => sink.SendAsync(new HangUp(), default));
        Assert.DoesNotContain(events, value => value is CallEnded or CallFailed);
        await Assert.ThrowsAsync<InvalidOperationException>(() => sink.SendAsync(new Dial("10.0.0.21", CallProtocol.H323, "MyStudio"), default));
        worker.Emit("call 1: DISCONNECTED (local-hangup)", true);
        Assert.Single(events.OfType<CallEnded>());
        worker.DeferCallConfirmation = false;
        await sink.SendAsync(new Dial("10.0.0.21", CallProtocol.H323, "MyStudio"), default);
    }

    [Fact]
    public async Task Worker_exit_during_pending_opal_hangup_releases_the_dead_process_reservation()
    {
        var worker = new Worker { DeferCallConfirmation = true, DeferHangupConfirmation = true };
        await using var sink = new OpalEngineCommandSink(new SharedOpalConsole(worker), EngineFamily.Cisco);
        await sink.StartAsync(default);
        await sink.SendAsync(new Dial("10.0.0.20", CallProtocol.Sip, "MyStudio"), default);
        var hangup = sink.SendAsync(new HangUp(), default);
        worker.Exit();
        await hangup.WaitAsync(TimeSpan.FromSeconds(2));
        await sink.ActivateProtocolAsync(CallProtocol.H323, default);
        worker.DeferCallConfirmation = false;
        await sink.SendAsync(new Dial("10.0.0.21", CallProtocol.H323, "MyStudio"), default);
        Assert.Equal("call 10.0.0.21", worker.Commands[^1]);
    }

    internal sealed class Worker : IProtocolConsole
    {
        private CallProtocol selected = CallProtocol.Sip;
        private long sequence = 10;
        public int Starts, Disposals;
        public bool RejectAudioProfiles, SkipAudioAcknowledgment, FailNextCall, DeferCallConfirmation, DeferHangupConfirmation, ThrowAfterCallConfirmation;
        public List<string> Commands { get; } = [];
        public CallProtocol Protocol => CallProtocol.Sip;
        public long ProcessGeneration => 1;
        public event EventHandler<ProtocolConsoleLine>? LineReceived;
        public event EventHandler<ProtocolConsoleGenerationEnded>? GenerationEnded;
        public Task StartAsync(CancellationToken token) { Starts++; return Task.CompletedTask; }
        public string? BlockCommand;
        public TaskCompletionSource BlockEntered { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource ReleaseBlock { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public Task WriteLineAsync(string command, CancellationToken token)
        {
            Commands.Add(command);
            if (command == BlockCommand) { BlockEntered.TrySetResult(); return ReleaseBlock.Task.WaitAsync(token); }
            if (command.StartsWith("protocol ")) selected = command.EndsWith("sip") ? CallProtocol.Sip : CallProtocol.H323;
            var p = selected == CallProtocol.Sip ? "sip" : "h323";
            if (command.StartsWith("audio-profile "))
            {
                if (RejectAudioProfiles)
                {
                    Emit($"{{\"schema\":1,\"type\":\"media-error\",\"sequence\":{++sequence},\"protocol\":\"{p}\",\"code\":\"audio-profile-invalid\",\"detail\":\"Profile could not be applied\"}}");
                    return Task.CompletedTask;
                }
                if (SkipAudioAcknowledgment) return Task.CompletedTask;
                var parts = command.Split(' ');
                Emit($"{{\"schema\":1,\"type\":\"audio-profile-applied\",\"sequence\":{++sequence},\"protocol\":\"{p}\",\"codec\":\"{parts[1]}\",\"channels\":{parts[2]},\"bitrateKbps\":{parts[3]},\"jitterMinimumMs\":{parts[4]},\"jitterMaximumMs\":{parts[5]},\"jitterInitialMs\":{(selected == CallProtocol.H323 ? parts[4] : parts[6])},\"sendBufferMs\":{parts[7]},\"packetMs\":{parts[8]},\"negotiated\":false}}");
            }
            if (command.StartsWith("network-video "))
            {
                var parts = command.Split(' ');
                Emit($"{{\"schema\":1,\"type\":\"network-video-applied\",\"sequence\":{++sequence},\"protocol\":\"{p}\",\"fps\":{parts[1]},\"bitrateKbps\":{parts[2]},\"content\":\"{parts[3]}\",\"negotiated\":false}}");
            }
            if (command.StartsWith("call "))
            {
                if (FailNextCall)
                {
                    FailNextCall = false;
                    Emit(selected == CallProtocol.Sip ? "startup error: call-setup-failed: No media" : "dial-error reason=call-setup-failed", true);
                }
                else if (!DeferCallConfirmation) Emit(selected == CallProtocol.Sip ? "call 1: CONFIRMED (OK)" : $"call-established remote=\"{command[5..]}\"", true);
                if (ThrowAfterCallConfirmation) return Task.FromException(new TimeoutException("Synthetic caller deadline after actual native confirmation."));
            }
            if (command == "hangup" && !DeferHangupConfirmation) Emit(selected == CallProtocol.Sip ? "call 1: DISCONNECTED (local-hangup)" : "call-cleared remote=\"10.0.0.20\" reason=local-hangup", true);
            return Task.CompletedTask;
        }
        public void Emit(string text, bool error = false)
        {
            if (text.StartsWith('{'))
            {
                using var json = System.Text.Json.JsonDocument.Parse(text);
                if (json.RootElement.TryGetProperty("sequence", out var value)) sequence = Math.Max(sequence, value.GetInt64());
            }
            LineReceived?.Invoke(this, new(text, error));
        }
        public ValueTask DisposeAsync() { Disposals++; GenerationEnded?.Invoke(this, new(1)); return ValueTask.CompletedTask; }
        public void Exit() => GenerationEnded?.Invoke(this, new(1));
    }
}
