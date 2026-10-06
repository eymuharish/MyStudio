using TeleStudio.Contracts;
using TeleStudio.Presentation;

namespace TeleStudio.Desktop.Tests;

public sealed class VideoEventRoutingTests
{
    [Theory]
    [InlineData(CallProtocol.Sip)]
    [InlineData(CallProtocol.H323)]
    public async Task Video_confirmation_survives_main_model_dispatch_and_unlocks_both_sources(CallProtocol protocol)
    {
        var commands = new Commands();
        var main = new MainViewModel(commands);
        var video = new InCallVideoViewModel(commands);
        main.ApplyEvent(new CallConnected("127.0.0.1", protocol, 0, 0, 0));
        video.UpdateAvailability(main.IsConnected, true);
        video.SelectDesktop("monitor-primary");
        await video.ToggleCameraAsync(default);
        await video.ToggleDesktopAsync(default);
        Assert.True(video.CameraPending);
        Assert.True(video.DesktopPending);
        var session = main.Session;
        var confirmation = new VideoStateChanged(true, true, true, true, VideoResolution.Hd, 1500,
            1280, 720, 1500, CameraOverlaySize.Medium, CameraOverlayPosition.BottomRight, 2, "", "");

        // MainWindow routes every event through MainViewModel before its video-specific handler.
        main.ApplyEvent(confirmation);
        video.UpdateAvailability(main.IsConnected, true);
        video.ApplyState(confirmation);

        Assert.Same(session, main.Session);
        Assert.False(video.CameraPending);
        Assert.False(video.DesktopPending);
        Assert.True(video.CanToggleCamera);
        Assert.True(video.CanToggleDesktop);
        Assert.Equal(1280, video.Confirmed.AppliedWidth);
        Assert.Equal(1500, video.Confirmed.AppliedBitrateKbps);
    }

    [Theory]
    [InlineData(CallProtocol.Sip)]
    [InlineData(CallProtocol.H323)]
    public void Local_preview_event_can_reach_presenter_without_changing_call_state(CallProtocol protocol)
    {
        var main = new MainViewModel(new Commands());
        main.ApplyEvent(new CallConnected("127.0.0.1", protocol, 0, 0, 0));
        var session = main.Session;
        var surface = new LocalSurfaceChanged(123, 1280, 720, 1);

        // Previously throws before MainWindow can attach this surface or acknowledge the producer.
        main.ApplyEvent(surface);

        Assert.Same(session, main.Session);
        Assert.Null(main.RemoteSurface);
        Assert.True(main.IsConnected);
    }

    private sealed class Commands : IEngineCommandSink
    {
        public Task SendAsync(EngineCommand command, CancellationToken cancellationToken) => Task.CompletedTask;
    }
}
