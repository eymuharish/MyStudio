using TeleStudio.Contracts;
using TeleStudio.Presentation;
namespace TeleStudio.Desktop.Tests;

public class InCallVideoViewModelTests
{
    private sealed class Commands : IEngineCommandSink
    {
        public List<EngineCommand> Sent = [];
        public Task SendAsync(EngineCommand command, CancellationToken token) { Sent.Add(command); return Task.CompletedTask; }
    }
    private static VideoStateChanged State(ulong revision, bool camera = false, bool desktop = false,
        bool active = false, string cameraError = "") => new(camera, desktop, camera && active, desktop && active,
            VideoResolution.Hd, null, 1280, 720, 1000, CameraOverlaySize.Medium, CameraOverlayPosition.BottomRight,
            revision, cameraError, "");

    [Fact]
    public void Log_only_network_notice_does_not_erase_a_real_capture_failure()
    {
        var vm = new InCallVideoViewModel(new Commands());
        vm.UpdateAvailability(true, true);
        vm.ApplyState(State(1, true, false, false, "permission denied"));
        vm.ApplyError(new MediaError("network-video-rejected", "Encoder rejected network profile"));
        Assert.Equal("permission denied", vm.Error);
        Assert.False(vm.Confirmed.CameraActive);
    }

    [Fact]
    public async Task Rejected_live_bitrate_retains_confirmed_video_without_error_or_pending_control()
    {
        var vm = new InCallVideoViewModel(new Commands());
        vm.UpdateAvailability(true, true); vm.ApplyState(State(1, true, true, true));
        await vm.SetPreferencesAsync(VideoResolution.Hd, 2000, default);
        vm.ApplyError(new MediaError("video-preferences-failed", "Encoder rejected bitrate change"));
        Assert.False(vm.PreferencesPending);
        Assert.Null(vm.Error);
        Assert.True(vm.Confirmed.CameraActive);
        Assert.True(vm.Confirmed.DesktopActive);
        Assert.Equal(1000, vm.Confirmed.AppliedBitrateKbps);
    }

    [Fact]
    public async Task Independent_sources_wait_for_matching_active_ack_and_reject_stale_state()
    {
        var commands = new Commands(); var vm = new InCallVideoViewModel(commands);
        vm.UpdateAvailability(true, true); vm.SelectDesktop("monitor"); vm.ApplyState(State(1));
        await vm.ToggleCameraAsync(default); await vm.ToggleDesktopAsync(default);
        Assert.Equal(new SetCameraEnabled(true, null), commands.Sent[0]);
        Assert.Equal(new SetDesktopEnabled(true, "monitor"), commands.Sent[1]);
        vm.ApplyState(State(2, true, true));
        Assert.True(vm.CameraPending); Assert.True(vm.DesktopPending);
        vm.ApplyState(State(3, true, false, true));
        Assert.False(vm.CameraPending); Assert.True(vm.DesktopPending);
        vm.ApplyState(State(2)); Assert.True(vm.Confirmed.CameraActive);
        Assert.Throws<InvalidOperationException>(() => vm.SelectCamera("different"));
        Assert.Throws<InvalidOperationException>(() => vm.SelectDesktop("different"));
        vm.UpdateAvailability(false, false); vm.ApplyState(State(99, true, true, true));
        Assert.False(vm.Confirmed.CameraActive); Assert.False(vm.CameraPending);
    }

    [Fact]
    public async Task Layout_and_preferences_stay_confirmed_until_matching_ack_or_specific_error()
    {
        var commands = new Commands(); var vm = new InCallVideoViewModel(commands);
        vm.UpdateAvailability(true, true); vm.ApplyState(State(1));
        await vm.SetOverlayAsync(CameraOverlaySize.Small, CameraOverlayPosition.TopLeft, default);
        await vm.SetPreferencesAsync(VideoResolution.Fhd, 2000, default);
        vm.ApplyState(State(2));
        Assert.True(vm.LayoutPending); Assert.True(vm.PreferencesPending);
        Assert.Equal(CameraOverlayPosition.BottomRight, vm.Confirmed.OverlayPosition);
        vm.ApplyError(new MediaError("camera-capture-failed", "failure"));
        Assert.True(vm.LayoutPending); Assert.True(vm.PreferencesPending);
        vm.ApplyError(new MediaError("video-preferences-failed", "resize rejected"));
        Assert.False(vm.PreferencesPending); Assert.True(vm.LayoutPending);
        Assert.Equal(1280, vm.Confirmed.AppliedWidth);
        vm.ApplyState(State(3) with { OverlaySize = CameraOverlaySize.Small, OverlayPosition = CameraOverlayPosition.TopLeft });
        Assert.False(vm.LayoutPending);
    }

    [Fact]
    public async Task Preview_and_initial_settings_never_enable_capture_and_source_error_settles_only_its_lane()
    {
        var commands = new Commands(); var vm = new InCallVideoViewModel(commands);
        vm.LoadSettings(new TeleStudio.Settings.AppSettings { CameraId = "real-id", VideoResolution = VideoResolution.Sd });
        vm.ToggleView(); vm.TogglePictureInPicture(); Assert.Empty(commands.Sent);
        vm.UpdateAvailability(true, true); vm.SelectDesktop("screen"); vm.ApplyState(State(1));
        await vm.ToggleCameraAsync(default); await vm.ToggleDesktopAsync(default);
        vm.ApplyState(State(2, true, true, false, "permission denied"));
        Assert.False(vm.CameraPending); Assert.True(vm.DesktopPending);
        Assert.False(vm.Confirmed.CameraActive);
    }

    [Fact]
    public async Task Recovery_clears_source_error_and_invalid_preferences_leave_requested_settings_unchanged()
    {
        var vm=new InCallVideoViewModel(new Commands()); vm.UpdateAvailability(true,true);
        vm.ApplyState(State(1,true,false,false,"permission denied")); Assert.Equal("permission denied",vm.Error);
        await vm.ToggleCameraAsync(default);
        vm.ApplyError(new MediaError("camera-capture-failed","late capture failure"));
        Assert.True(vm.CameraPending);
        vm.ApplyState(State(2,true,true,false,"permission denied")); Assert.True(vm.CameraPending);
        vm.ApplyState(State(3)); Assert.Null(vm.Error); Assert.False(vm.CameraPending);
        await Assert.ThrowsAsync<ArgumentOutOfRangeException>(()=>vm.SetPreferencesAsync(VideoResolution.Fhd,12,default));
        Assert.Equal(VideoResolution.Auto,vm.RequestedResolution);
    }

    [Fact]
    public void Language_copy_preserves_video_and_unrelated_settings()
    {
        var source=new TeleStudio.Settings.AppSettings {CameraId="real-id",VideoResolution=VideoResolution.Fhd,
            VideoBitrateKbps=2500,CameraOverlaySize=CameraOverlaySize.Small,CameraOverlayPosition=CameraOverlayPosition.TopLeft,
            MicrophoneGain=120,RecentDestinations=["127.0.0.1"]};
        var copy=TeleStudio.Settings.AppSettingsCopy.Copy(source,"EssaStudio","en");
        Assert.Equal(source.VideoResolution,copy.VideoResolution); Assert.Equal(2500,copy.VideoBitrateKbps);
        Assert.Equal(CameraOverlayPosition.TopLeft,copy.CameraOverlayPosition); Assert.Equal(120,copy.MicrophoneGain);
        Assert.Equal(source.RecentDestinations,copy.RecentDestinations); Assert.Equal("real-id",copy.CameraId);
    }
}
