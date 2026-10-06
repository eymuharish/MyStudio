using TeleStudio.Contracts;
using TeleStudio.Presentation;
using System.Collections.Concurrent;
using System.Runtime.InteropServices;
using System.Reflection;
using System.Diagnostics;

namespace TeleStudio.Desktop.Tests;

public sealed class InCallAudioViewModelTests
{
    [Fact]
    public async Task System_audio_waits_for_async_activation_acknowledgement_and_repeated_clicks_cannot_restart_capture()
    {
        var sink = new FakeSink(); var timeouts = new FakeConfirmationTimeoutScheduler();
        var vm = new InCallAudioViewModel(sink, new FakeScheduler(), timeouts);
        vm.UpdateAvailability(true, true, true, true, true);
        await vm.ToggleSystemAudioMuteAsync(default);
        Assert.Equal(TimeSpan.FromSeconds(6), Assert.Single(timeouts.Delays));
        Assert.True(vm.IsSystemAudioPending); Assert.False(vm.ConfirmedSystemAudioEnabled);
        await Assert.ThrowsAsync<InvalidOperationException>(() => vm.ToggleSystemAudioMuteAsync(default));
        Assert.Equal(new SetSystemAudioEnabled(true), Assert.Single(sink.Commands));
        vm.ApplySystemAudioConfirmation(new SystemAudioChanged(false));
        Assert.True(vm.IsSystemAudioPending); Assert.False(vm.CanToggleSystemAudioMute);
        vm.ApplySystemAudioConfirmation(new SystemAudioChanged(true));
        Assert.True(vm.ConfirmedSystemAudioEnabled); Assert.True(vm.CanToggleSystemAudioMute); Assert.False(vm.IsSystemAudioPending);
        await vm.ToggleSystemAudioMuteAsync(default);
        Assert.True(vm.ConfirmedSystemAudioEnabled);
        Assert.Equal(new SetSystemAudioEnabled(false), sink.Commands.Last());
        vm.ApplySystemAudioConfirmation(new SystemAudioChanged(true));
        Assert.True(vm.IsSystemAudioPending); Assert.False(vm.CanToggleSystemAudioMute);
        vm.ApplySystemAudioConfirmation(new SystemAudioChanged(false));
        Assert.False(vm.ConfirmedSystemAudioEnabled); Assert.True(vm.CanToggleSystemAudioMute);
    }
    [Fact]
    public void Microphone_controls_remain_accessible_without_room_mixer_capability()
    {
        var vm=new InCallAudioViewModel(new FakeSink(),new FakeScheduler());
        vm.UpdateAvailability(true,true,true,false,false);
        Assert.True(vm.CanOpenAudioMixer);
        Assert.False(vm.CanOpenRoomMixer);
        vm.UpdateAvailability(false,false,false,false,false);
        Assert.False(vm.CanOpenAudioMixer);
    }
    [Fact]
    public async Task Room_noise_reduction_waits_for_confirmation_and_reverts_on_rejection()
    {
        var sink=new FakeSink(); var vm=new InCallAudioViewModel(sink,new FakeScheduler());
        vm.UpdateAvailability(true,true,true,true,true);
        await vm.SetRoomMixerAsync(100,0,0,0,default,15);
        Assert.Equal(0,vm.ConfirmedRoomNoiseReductionDb);
        Assert.Equal(15,vm.PendingRoomNoiseReductionDb);
        Assert.Equal(new SetRoomMixer(100,0,0,0,15),sink.Commands.Last());
        vm.ApplyRoomMixerConfirmation(new RoomMixerChanged(100,0,0,0,15));
        Assert.Equal(15,vm.ConfirmedRoomNoiseReductionDb);
        Assert.False(vm.IsRoomMixerPending);
        await vm.SetRoomMixerAsync(100,0,0,0,default,30);
        vm.ApplyMediaError(new MediaError("room-mixer-rejected","unavailable"));
        Assert.Equal(15,vm.PendingRoomNoiseReductionDb);
        Assert.False(vm.IsRoomMixerPending);
        vm.UpdateAvailability(false,false);
        Assert.Equal(0,vm.ConfirmedRoomNoiseReductionDb);
    }
    [Theory]
    [InlineData(-1)]
    [InlineData(31)]
    public async Task Room_noise_reduction_rejects_out_of_range_values(int value)
    {
        var sink=new FakeSink(); var vm=new InCallAudioViewModel(sink,new FakeScheduler());
        vm.UpdateAvailability(true,true,true,true,true);
        await Assert.ThrowsAsync<ArgumentOutOfRangeException>(()=>vm.SetRoomMixerAsync(100,0,0,0,default,value));
        Assert.Empty(sink.Commands);
    }
    [Fact]
    public async Task Independent_mute_lanes_are_capability_gated_and_do_not_block_each_other()
    {
        var sink = new FakeSink();
        var vm = new InCallAudioViewModel(sink, new FakeScheduler());
        vm.UpdateAvailability(true, false, microphoneReady: true, systemAudioReady: true, roomMixerReady: true);

        await vm.ToggleMicrophoneMuteAsync(default);
        Assert.True(vm.PendingMicrophoneMuted);
        Assert.True(vm.IsMicrophoneMutePending);
        Assert.False(vm.PendingSystemAudioMuted);
        Assert.False(vm.PendingRoomMuted);
        Assert.False(vm.CanToggleMicrophoneMute);
        Assert.True(vm.CanToggleSystemAudioMute);
        Assert.True(vm.CanToggleRoomMute);

        await vm.ToggleRoomMuteAsync(default);
        Assert.True(vm.PendingRoomMuted);
        Assert.True(vm.IsRoomMutePending);
        Assert.Equal([new SetMicrophoneMuted(true), new SetRoomAudioMuted(true)], sink.Commands);
    }

    [Fact]
    public async Task Confirmations_and_targeted_errors_clear_only_the_owned_pending_lane()
    {
        var sink = new FakeSink();
        var vm = new InCallAudioViewModel(sink, new FakeScheduler());
        vm.UpdateAvailability(true, false, true, true, true);
        await vm.ToggleMicrophoneMuteAsync(default);
        await vm.ToggleSystemAudioMuteAsync(default);
        await vm.ToggleRoomMuteAsync(default);

        vm.ApplySystemAudioConfirmation(new SystemAudioChanged(true));
        Assert.True(vm.ConfirmedSystemAudioEnabled);
        Assert.False(vm.ConfirmedSystemAudioMuted);
        Assert.False(vm.IsSystemAudioPending);
        Assert.True(vm.IsMicrophoneMutePending);
        Assert.True(vm.IsRoomMutePending);

        vm.ApplyMediaError(new MediaError("room-mixer-rejected", "not available"));
        Assert.False(vm.IsRoomMutePending);
        Assert.False(vm.PendingRoomMuted);
        Assert.True(vm.IsMicrophoneMutePending);
        Assert.False(vm.ConfirmedMicrophoneMuted);
        Assert.Equal(new MediaError("room-mixer-rejected", "not available"), vm.LastError);

        vm.ApplyMicrophoneMuteConfirmation(new MicrophoneMuteChanged(true));
        Assert.True(vm.ConfirmedMicrophoneMuted);
        Assert.False(vm.IsMicrophoneMutePending);
    }

    [Fact]
    public async Task Shared_room_error_is_correlated_to_the_oldest_pending_room_command_only()
    {
        var timeouts = new FakeConfirmationTimeoutScheduler();
        var vm = ReadyAudioViewModel(timeouts);
        await vm.ToggleRoomMuteAsync(default);
        await vm.SetRoomMixerAsync(180, 3, 4, 1, default);

        vm.ApplyMediaError(new MediaError("room-mixer-rejected", "room-mixer-unavailable"));

        Assert.False(vm.IsRoomMutePending);
        Assert.False(vm.PendingRoomMuted);
        Assert.True(vm.IsRoomMixerPending);
        Assert.Equal(180, vm.PendingRoomMasterPercent);
        Assert.True(timeouts.IsCancelled(0));
        Assert.False(timeouts.IsCancelled(1));
    }

    [Fact]
    public async Task Shared_room_error_preserves_pending_mute_when_mixer_command_was_first()
    {
        var timeouts = new FakeConfirmationTimeoutScheduler();
        var vm = ReadyAudioViewModel(timeouts);
        await vm.SetRoomMixerAsync(180, 3, 4, 1, default);
        await vm.ToggleRoomMuteAsync(default);

        vm.ApplyMediaError(new MediaError("room-mixer-rejected", "room-mixer-unavailable"));

        Assert.False(vm.IsRoomMixerPending);
        Assert.Equal(100, vm.PendingRoomMasterPercent);
        Assert.True(vm.IsRoomMutePending);
        Assert.True(vm.PendingRoomMuted);
        Assert.True(timeouts.IsCancelled(0));
        Assert.False(timeouts.IsCancelled(1));
    }

    [Fact]
    public async Task Targeted_errors_cancel_only_their_confirmation_timer()
    {
        var timeouts = new FakeConfirmationTimeoutScheduler();
        var vm = ReadyAudioViewModel(timeouts);
        await vm.ToggleMicrophoneMuteAsync(default);
        await vm.ToggleSystemAudioMuteAsync(default);

        vm.ApplyMediaError(new MediaError("microphone-mute-rejected", "not available"));

        Assert.True(timeouts.IsCancelled(0));
        Assert.False(timeouts.IsCancelled(1));
        Assert.False(vm.IsMicrophoneMutePending);
        Assert.True(vm.IsSystemAudioPending);

        vm.ApplyMediaError(new MediaError("system-audio-rejected", "not available"));
        Assert.True(timeouts.IsCancelled(1));
        Assert.False(vm.IsSystemAudioPending);
    }

    [Fact]
    public async Task H323_audio_not_ready_codes_resolve_only_the_owning_lane()
    {
        var timeouts = new FakeConfirmationTimeoutScheduler();
        var vm = ReadyAudioViewModel(timeouts);
        await vm.ToggleMicrophoneMuteAsync(default);
        await vm.ToggleSystemAudioMuteAsync(default);

        vm.ApplyMediaError(new MediaError("mic-mute-audio-not-ready", "audio-control-unavailable"));

        Assert.False(vm.IsMicrophoneMutePending);
        Assert.True(vm.IsSystemAudioPending);
        Assert.True(timeouts.IsCancelled(0));
        Assert.False(timeouts.IsCancelled(1));

        vm.ApplyMediaError(new MediaError("system-audio-audio-not-ready", "audio-control-unavailable"));

        Assert.False(vm.IsSystemAudioPending);
        Assert.True(timeouts.IsCancelled(1));
    }

    [Fact]
    public async Task Microphone_confirmation_timeout_rolls_back_only_microphone_lane()
    {
        var timeouts = new FakeConfirmationTimeoutScheduler();
        var vm = ReadyAudioViewModel(timeouts);
        await vm.ToggleMicrophoneMuteAsync(default);
        await vm.ToggleSystemAudioMuteAsync(default);

        timeouts.Fire(0);

        Assert.False(vm.IsMicrophoneMutePending);
        Assert.False(vm.PendingMicrophoneMuted);
        Assert.True(vm.IsSystemAudioPending);
    }

    [Fact]
    public async Task System_audio_confirmation_timeout_rolls_back_only_system_lane()
    {
        var timeouts = new FakeConfirmationTimeoutScheduler();
        var vm = ReadyAudioViewModel(timeouts);
        await vm.ToggleSystemAudioMuteAsync(default);
        await vm.ToggleRoomMuteAsync(default);

        timeouts.Fire(0);

        Assert.False(vm.IsSystemAudioPending);
        Assert.True(vm.ConfirmedSystemAudioMuted);
        Assert.True(vm.IsRoomMutePending);
    }

    [Fact]
    public async Task Room_mute_confirmation_timeout_rolls_back_only_room_mute_lane()
    {
        var timeouts = new FakeConfirmationTimeoutScheduler();
        var vm = ReadyAudioViewModel(timeouts);
        await vm.ToggleRoomMuteAsync(default);
        await vm.SetRoomMixerAsync(180, 3, 4, 1, default);

        timeouts.Fire(0);

        Assert.False(vm.IsRoomMutePending);
        Assert.False(vm.PendingRoomMuted);
        Assert.True(vm.IsRoomMixerPending);
        Assert.Equal(180, vm.PendingRoomMasterPercent);
    }

    [Fact]
    public async Task Room_mixer_confirmation_timeout_rolls_back_only_mixer_lane()
    {
        var timeouts = new FakeConfirmationTimeoutScheduler();
        var vm = ReadyAudioViewModel(timeouts);
        await vm.SetRoomMixerAsync(180, 3, 4, 1, default);
        await vm.ToggleRoomMuteAsync(default);

        timeouts.Fire(0);

        Assert.False(vm.IsRoomMixerPending);
        Assert.Equal(100, vm.PendingRoomMasterPercent);
        Assert.True(vm.IsRoomMutePending);
    }

    [Fact]
    public async Task Confirmation_cancels_timeout_and_stale_generation_cannot_clear_a_new_request()
    {
        var timeouts = new FakeConfirmationTimeoutScheduler();
        var vm = ReadyAudioViewModel(timeouts);
        await vm.ToggleMicrophoneMuteAsync(default);
        vm.ApplyMicrophoneMuteConfirmation(new MicrophoneMuteChanged(true));
        await vm.ToggleMicrophoneMuteAsync(default);

        timeouts.ForceFire(0);

        Assert.True(vm.IsMicrophoneMutePending);
        Assert.False(vm.PendingMicrophoneMuted);
        Assert.True(timeouts.IsCancelled(0));
    }

    [Fact]
    public async Task Call_reset_cancels_all_confirmation_timeouts()
    {
        var timeouts = new FakeConfirmationTimeoutScheduler();
        var vm = ReadyAudioViewModel(timeouts);
        await vm.ToggleMicrophoneMuteAsync(default);
        await vm.ToggleSystemAudioMuteAsync(default);
        await vm.ToggleRoomMuteAsync(default);
        await vm.SetRoomMixerAsync(180, 3, 4, 1, default);

        vm.UpdateAvailability(false, false);

        Assert.All(Enumerable.Range(0, 4), index => Assert.True(timeouts.IsCancelled(index)));
        timeouts.ForceFireAll();
        Assert.False(vm.IsMicrophoneMutePending);
        Assert.False(vm.IsSystemAudioPending);
        Assert.False(vm.IsRoomMutePending);
        Assert.False(vm.IsRoomMixerPending);
    }

    [Fact]
    public void Meter_updates_raise_only_the_dedicated_meter_notification()
    {
        var vm = new InCallAudioViewModel(new FakeSink(), new FakeScheduler());
        var properties = new List<string?>();
        var meters = 0;
        vm.PropertyChanged += (_, args) => properties.Add(args.PropertyName);
        vm.RoomMeterChanged += (_, _) => meters++;

        vm.ApplyRoomMeter(new RoomAudioMeterChanged(32767, 29204, 1234, true));

        Assert.Empty(properties);
        Assert.Equal(1, meters);
    }

    [Fact]
    public async Task Room_mixer_accepts_exact_bounds_and_projects_confirmed_meter_warning()
    {
        var sink = new FakeSink();
        var vm = new InCallAudioViewModel(sink, new FakeScheduler());
        vm.UpdateAvailability(true, false, false, false, true);

        await vm.SetRoomMixerAsync(300, -12, 12, 0, default);
        Assert.True(vm.IsRoomMixerPending);
        Assert.Equal(new SetRoomMixer(300, -12, 12, 0), Assert.Single(sink.Commands));
        await Assert.ThrowsAsync<ArgumentOutOfRangeException>(() => vm.SetRoomMixerAsync(301, 0, 0, 0, default));
        await Assert.ThrowsAsync<ArgumentOutOfRangeException>(() => vm.SetRoomMixerAsync(100, -13, 0, 0, default));

        vm.ApplyRoomMixerConfirmation(new RoomMixerChanged(300, -12, 12, 0));
        vm.ApplyRoomMeter(new RoomAudioMeterChanged(32767, 29204, 1234, true));
        Assert.Equal(300, vm.ConfirmedRoomMasterPercent);
        Assert.Equal(-12, vm.ConfirmedRoomBassDb);
        Assert.Equal(12, vm.ConfirmedRoomVoiceDb);
        Assert.False(vm.IsRoomMixerPending);
        Assert.Equal(32767, vm.RoomPreLimiterPeakQ15);
        Assert.Equal(29204, vm.RoomPostLimiterPeakQ15);
        Assert.Equal(1234, vm.RoomLimiterGainReductionQ15);
        Assert.True(vm.IsLimiterWarningVisible);
    }

    [Fact]
    public async Task Confirmed_desktop_share_auto_requests_system_audio_once_and_manual_mute_blocks_reenable_until_reset()
    {
        var sink = new FakeSink();
        var vm = new InCallAudioViewModel(sink, new FakeScheduler());
        vm.UpdateAvailability(true, false, true, true, true);

        await vm.HandleConfirmedSourceChangeAsync(true, default);
        await vm.HandleConfirmedSourceChangeAsync(true, default);
        Assert.Equal(new SetSystemAudioEnabled(true), Assert.Single(sink.Commands));
        vm.ApplySystemAudioConfirmation(new SystemAudioChanged(true));

        await vm.ToggleSystemAudioMuteAsync(default);
        vm.ApplySystemAudioConfirmation(new SystemAudioChanged(false));
        await vm.HandleConfirmedSourceChangeAsync(true, default);
        Assert.Equal(2, sink.Commands.Count);
        Assert.Equal(new SetSystemAudioEnabled(false), sink.Commands[1]);

        await vm.HandleConfirmedSourceChangeAsync(false, default);
        await vm.HandleConfirmedSourceChangeAsync(true, default);
        Assert.Equal(new SetSystemAudioEnabled(true), sink.Commands[2]);
    }

    [Fact]
    public void Defaults_bounds_and_reset_are_truthful()
    {
        var vm = new InCallAudioViewModel(new FakeSink(), new FakeScheduler());
        Assert.Equal(100, vm.ConfirmedRoomVolume);
        Assert.Equal(100, vm.ConfirmedMicrophoneGain);
        Assert.True(vm.ConfirmedNoiseIsolation);
        Assert.Throws<ArgumentOutOfRangeException>(() => vm.SetPendingRoomVolume(101));
        Assert.Throws<ArgumentOutOfRangeException>(() => vm.SetPendingMicrophoneGain(151));
        vm.ResetRoomVolume();
        vm.ResetMicrophoneGain();
        Assert.Equal(100, vm.PendingRoomVolume);
        Assert.Equal(100, vm.PendingMicrophoneGain);
    }

    [Fact]
    public async Task Panel_gates_on_connected_readiness_and_debounces_slider_writes()
    {
        var sink = new FakeSink();
        var scheduler = new FakeScheduler();
        var vm = new InCallAudioViewModel(sink, scheduler);
        vm.UpdateAvailability(true, false);
        Assert.False(vm.IsEnabled);
        vm.UpdateAvailability(true, true);

        vm.SetPendingRoomVolume(75);
        vm.SetPendingRoomVolume(74);
        Assert.Empty(sink.Commands);
        await scheduler.RunLastAsync();
        Assert.Single(sink.Commands);
        vm.ApplyConfirmation(new AudioControlsChanged(74, 100, true));
        await vm.CommitMicrophoneGainAsync(120, default);
        Assert.Equal(2, sink.Commands.Count);
        Assert.Equal(74, vm.ConfirmedRoomVolume);
    }

    [Fact]
    public async Task Confirmation_is_distinct_and_rejection_or_readiness_loss_snaps_back()
    {
        var sink = new FakeSink { Fail = true };
        var vm = new InCallAudioViewModel(sink, new FakeScheduler());
        vm.UpdateAvailability(true, true);
        await Assert.ThrowsAsync<IOException>(() => vm.CommitRoomVolumeAsync(70, default));
        Assert.Equal(100, vm.PendingRoomVolume);
        vm.ApplyConfirmation(new AudioControlsChanged(70, 130, false));
        Assert.Equal(70, vm.ConfirmedRoomVolume);
        Assert.Equal(130, vm.ConfirmedMicrophoneGain);
        Assert.False(vm.ConfirmedNoiseIsolation);
        vm.SetPendingRoomVolume(60);
        vm.UpdateAvailability(false, false);
        Assert.Equal(70, vm.PendingRoomVolume);
    }

    [Fact]
    public async Task Mute_and_recording_do_not_change_microphone_gain_or_isolation_semantics()
    {
        var sink = new FakeSink();
        var vm = new InCallAudioViewModel(sink, new FakeScheduler());
        vm.UpdateAvailability(true, true);
        await vm.ToggleNoiseIsolationAsync(default);
        var command = Assert.IsType<SetAudioControls>(Assert.Single(sink.Commands));
        Assert.Equal(100, command.MicrophoneGain);
        Assert.False(command.NoiseIsolation);
        Assert.Contains("Speex", vm.NoiseIsolationDescription, StringComparison.Ordinal);
        Assert.Equal("LeftToRight", vm.NumericFlowDirection);
    }

    private static string FindRepositoryRoot()
    {
        var directory = new DirectoryInfo(AppContext.BaseDirectory);
        while (directory is not null && !File.Exists(Path.Combine(directory.FullName, "TeleStudio.slnx"))) directory = directory.Parent;
        return directory?.FullName ?? throw new DirectoryNotFoundException("repository-root-missing");
    }

    private sealed class FakeSink : IEngineCommandSink
    {
        public List<EngineCommand> Commands { get; } = [];
        public bool Fail { get; set; }
        public Task SendAsync(EngineCommand command, CancellationToken cancellationToken)
        {
            if (Fail) throw new IOException("rejected");
            Commands.Add(command);
            return Task.CompletedTask;
        }
    }

    private sealed class FakeScheduler : IAudioCommitScheduler
    {
        private Func<Task>? _last;
        public void Schedule(Func<Task> action) => _last = action;
        public void Cancel() => _last = null;
        public Task RunLastAsync() { var action = _last; _last = null; return action?.Invoke() ?? Task.CompletedTask; }
    }

    private static InCallAudioViewModel ReadyAudioViewModel(FakeConfirmationTimeoutScheduler timeouts)
    {
        var vm = new InCallAudioViewModel(new FakeSink(), new FakeScheduler(), timeouts);
        vm.UpdateAvailability(true, false, true, true, true);
        return vm;
    }

    private sealed class FakeConfirmationTimeoutScheduler : IAudioConfirmationTimeoutScheduler
    {
        private readonly List<Entry> _entries = [];
        public List<TimeSpan> Delays { get; } = [];
        public IDisposable Schedule(TimeSpan delay, Action action)
        {
            Assert.Contains(delay, new[] { TimeSpan.FromSeconds(2), TimeSpan.FromSeconds(6) });
            Delays.Add(delay);
            var entry = new Entry(action);
            _entries.Add(entry);
            return entry;
        }
        public void Fire(int index) => _entries[index].Fire(honorCancellation: true);
        public void ForceFire(int index) => _entries[index].Fire(honorCancellation: false);
        public void ForceFireAll() { foreach (var entry in _entries) entry.Fire(honorCancellation: false); }
        public bool IsCancelled(int index) => _entries[index].Cancelled;

        private sealed class Entry(Action action) : IDisposable
        {
            public bool Cancelled { get; private set; }
            public void Dispose() => Cancelled = true;
            public void Fire(bool honorCancellation) { if (!honorCancellation || !Cancelled) action(); }
        }
    }
}
