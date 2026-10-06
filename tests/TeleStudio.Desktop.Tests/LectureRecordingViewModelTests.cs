using TeleStudio.Contracts;
using TeleStudio.Presentation;

namespace TeleStudio.Desktop.Tests;

public sealed class LectureRecordingViewModelTests
{
    [Fact]
    public async Task Sheet_refreshes_catalog_selects_confirmed_monitor_and_never_starts_implicitly()
    {
        var sink = new FakeSink();
        var catalog = new FakeCatalog();
        var vm = Create(sink, catalog);
        vm.UpdateCallState(true, true, false, "monitor-two", false);

        await vm.OpenSheetAsync(default);

        Assert.True(vm.IncludeMicrophone);
        Assert.False(vm.IncludeRoomAudio);
        Assert.Equal("monitor-two", vm.SelectedItem!.Id);
        Assert.Empty(sink.Commands);
        Assert.Contains(vm.Items, item => item.Kind == CaptureItemKind.Window && item.Name == "Slides");
    }

    [Fact]
    public async Task Sheet_falls_back_only_to_primary_monitor_and_refresh_never_retargets_stale_selection()
    {
        var sink = new FakeSink();
        var catalog = new FakeCatalog();
        var vm = Create(sink, catalog);
        vm.UpdateCallState(true, true, false, "missing", false);
        await vm.OpenSheetAsync(default);
        Assert.Equal("monitor-primary", vm.SelectedItem!.Id);

        catalog.Items = [new("window-only", CaptureItemKind.Window, "Only window", 800, 600)];
        await vm.RefreshAsync(default);
        Assert.Null(vm.SelectedItem);
        Assert.Contains("no longer", vm.ErrorMessage, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public async Task Explicit_record_locks_target_and_options_once_and_confirmation_drives_state()
    {
        var sink = new FakeSink();
        var vm = Create(sink, new FakeCatalog());
        vm.UpdateCallState(true, true, false, null, false);
        await vm.OpenSheetAsync(default);
        vm.IncludeRoomAudio = true;
        await vm.StartAsync(default);
        await vm.StartAsync(default);

        var command = Assert.IsType<StartLectureRecording>(Assert.Single(sink.Commands));
        Assert.Equal("monitor-primary", command.SourceId);
        Assert.True(command.IncludeMicrophone);
        Assert.True(command.IncludeRoomAudio);
        Assert.Equal(RecordingState.Idle, vm.State);
        Assert.True(vm.OptionsLocked);

        vm.ApplyState(new RecordingStateChanged(RecordingState.Starting, true, true, 0, ""));
        Assert.False(vm.IsRecordingIndicatorVisible);
        vm.ApplyState(new RecordingStateChanged(RecordingState.Recording, true, true, 1000, ""));
        Assert.True(vm.IsRecordingIndicatorVisible);
        Assert.Equal(1000, vm.ElapsedMs);
    }

    [Fact]
    public async Task Stop_is_deduplicated_and_finalizing_is_locked()
    {
        var sink = new FakeSink();
        var vm = Create(sink, new FakeCatalog());
        vm.UpdateCallState(true, true, false, null, false);
        await vm.OpenSheetAsync(default);
        await vm.StartAsync(default);
        vm.ApplyState(new RecordingStateChanged(RecordingState.Starting, true, false, 0, ""));
        vm.ApplyState(new RecordingStateChanged(RecordingState.Recording, true, false, 20, ""));
        await vm.StopAsync(default);
        await vm.StopAsync(default);
        Assert.Single(sink.Commands.OfType<StopLectureRecording>());
        vm.ApplyState(new RecordingStateChanged(RecordingState.Finalizing, true, false, 20, ""));
        Assert.False(vm.CanStop);
        Assert.True(vm.OptionsLocked);
    }

    [Fact]
    public async Task Call_end_keeps_detached_finalization_and_safe_file_actions()
    {
        var files = new FakeFiles();
        var vm = Create(new FakeSink(), new FakeCatalog(), files);
        vm.UpdateCallState(true, true, false, null, false);
        await vm.OpenSheetAsync(default);
        await vm.StartAsync(default);
        vm.ApplyState(new RecordingStateChanged(RecordingState.Starting, true, false, 0, ""));
        vm.ApplyState(new RecordingStateChanged(RecordingState.Recording, true, false, 50, ""));
        vm.UpdateCallState(false, false, true, null, false);
        vm.ApplyState(new RecordingStateChanged(RecordingState.Finalizing, true, false, 60, ""));
        vm.ApplyState(new RecordingStateChanged(RecordingState.Completed, true, false, 60, "EssaStudio-20260901-010203.mp4"));
        Assert.True(vm.CanOpenFile);
        Assert.True(vm.OpenFile());
        Assert.Equal("EssaStudio-20260901-010203.mp4", files.OpenedFile);
        Assert.False(vm.ApplyState(new RecordingStateChanged(RecordingState.Starting, true, false, 0, "")));
        Assert.Equal(RecordingState.Completed, vm.State);
    }

    [Fact]
    public async Task Explicit_second_operation_resets_elapsed_and_accepts_starting_zero()
    {
        var sink = new FakeSink();
        var clock = new FakeClock();
        var timer = new FakeTimer();
        var vm = Create(sink, new FakeCatalog(), clock: clock, timer: timer);
        vm.UpdateCallState(true, true, false, null, false);
        await vm.OpenSheetAsync(default);
        await vm.StartAsync(default);
        Assert.True(vm.ApplyState(new RecordingStateChanged(RecordingState.Starting, true, false, 0, "")));
        Assert.True(vm.ApplyState(new RecordingStateChanged(RecordingState.Recording, true, false, 4_000, "")));
        Assert.True(vm.ApplyState(new RecordingStateChanged(RecordingState.Finalizing, true, false, 4_000, "")));
        Assert.True(vm.ApplyState(new RecordingStateChanged(RecordingState.Completed, true, false, 4_000, "first.mp4")));

        await vm.OpenSheetAsync(default);
        await vm.StartAsync(default);
        Assert.True(vm.ApplyState(new RecordingStateChanged(RecordingState.Starting, true, false, 0, "")));
        Assert.Equal(RecordingState.Starting, vm.State);
        Assert.Equal(0, vm.ElapsedMs);
        Assert.False(vm.CanOpenFile);
    }

    [Fact]
    public async Task Confirmed_finalizing_behind_interpolated_display_is_accepted_without_elapsed_regression()
    {
        var clock = new FakeClock { Milliseconds = 1_000 };
        var timer = new FakeTimer();
        var vm = Create(new FakeSink(), new FakeCatalog(), clock: clock, timer: timer);
        vm.UpdateCallState(true, true, false, null, false);
        await vm.OpenSheetAsync(default);
        await vm.StartAsync(default);
        vm.ApplyState(new RecordingStateChanged(RecordingState.Starting, true, false, 0, ""));
        vm.ApplyState(new RecordingStateChanged(RecordingState.Recording, true, false, 1_000, ""));
        clock.Milliseconds = 1_600;
        timer.Tick();
        Assert.Equal(1_600, vm.ElapsedMs);

        Assert.True(vm.ApplyState(new RecordingStateChanged(RecordingState.Finalizing, true, false, 1_100, "")));
        Assert.Equal(RecordingState.Finalizing, vm.State);
        Assert.Equal(1_600, vm.ElapsedMs);
    }

    [Fact]
    public async Task Muted_transmission_warning_is_non_color_and_errors_do_not_fail_call()
    {
        var vm = Create(new FakeSink(), new FakeCatalog());
        vm.UpdateCallState(true, true, false, null, true);
        await vm.OpenSheetAsync(default);
        Assert.True(vm.IsMuteRecordingWarningVisible);
        Assert.Contains("muted", vm.MuteRecordingWarning, StringComparison.OrdinalIgnoreCase);
        vm.ApplyError(new RecordingError("capture-target-removed", "Select the display again.", false, ""));
        Assert.Contains("Select", vm.ErrorMessage, StringComparison.Ordinal);
        Assert.True(vm.IsCallConnected);
    }

    [Fact]
    public async Task Start_rejection_unlocks_options_and_allows_the_sheet_to_close()
    {
        var vm = Create(new FakeSink(), new FakeCatalog());
        vm.UpdateCallState(true, true, false, null, false);
        await vm.OpenSheetAsync(default);
        await vm.StartAsync(default);
        Assert.True(vm.OptionsLocked);

        vm.ApplyError(new RecordingError("capture-target-removed", "Select the display again.", false, ""));

        Assert.False(vm.OptionsLocked);
        Assert.True(vm.CanRecord);
        vm.CloseUnstartedSheet();
        Assert.False(vm.IsSheetOpen);
    }

    [Fact]
    public async Task Active_recording_sheet_can_close_and_reopen_without_restarting()
    {
        var sink = new FakeSink();
        var vm = Create(sink, new FakeCatalog());
        vm.UpdateCallState(true, true, false, null, false);
        await vm.OpenSheetAsync(default);
        await vm.StartAsync(default);
        vm.ApplyState(new RecordingStateChanged(RecordingState.Starting, true, false, 0, ""));
        vm.ApplyState(new RecordingStateChanged(RecordingState.Recording, true, false, 20, ""));

        vm.CloseUnstartedSheet();
        Assert.False(vm.IsSheetOpen);
        Assert.True(vm.CanOpenSheet);
        await vm.OpenSheetAsync(default);
        Assert.True(vm.IsSheetOpen);
        Assert.Single(sink.Commands.OfType<StartLectureRecording>());

        vm.ApplyState(new RecordingStateChanged(RecordingState.Finalizing, true, false, 20, ""));
        vm.CloseUnstartedSheet();
        Assert.False(vm.IsSheetOpen);
        Assert.True(vm.CanOpenSheet);
    }

    private static LectureRecordingViewModel Create(FakeSink sink, FakeCatalog catalog, FakeFiles? files = null,
        FakeClock? clock = null, FakeTimer? timer = null) =>
        new(sink, catalog, clock ?? new FakeClock(), timer ?? new FakeTimer(), files ?? new FakeFiles());

    private sealed class FakeSink : IEngineCommandSink
    {
        public List<EngineCommand> Commands { get; } = [];
        public Task SendAsync(EngineCommand command, CancellationToken cancellationToken) { Commands.Add(command); return Task.CompletedTask; }
    }

    private sealed class FakeCatalog : ICaptureCatalog
    {
        public IReadOnlyList<CaptureCatalogItem> Items { get; set; } =
        [
            new("monitor-primary", CaptureItemKind.Monitor, "Primary display", 1920, 1080),
            new("monitor-two", CaptureItemKind.Monitor, "Second display", 1280, 720),
            new("window-slides", CaptureItemKind.Window, "Slides", 1600, 900),
        ];
        public Task<CaptureCatalogSnapshot> RefreshAsync(CancellationToken cancellationToken) =>
            Task.FromResult(new CaptureCatalogSnapshot(true, Items, null));
    }

    private sealed class FakeClock : IRecordingClock
    {
        public long Milliseconds { get; set; } = 1000;
        public long GetMilliseconds() => Milliseconds;
    }
    private sealed class FakeTimer : IRecordingTimer
    {
        private Action? _tick;
        public void Start(Action tick) => _tick = tick;
        public void Stop() => _tick = null;
        public void Tick() => _tick?.Invoke();
        public void Dispose() { }
    }
    private sealed class FakeFiles : IRecordingFileActionsSurface
    {
        public string? OpenedFile { get; private set; }
        public bool OpenFile(string basename) { OpenedFile = basename; return true; }
        public bool OpenFolder() => true;
    }
}
