using System.Text;
using TeleStudio.Contracts;
namespace TeleStudio.Desktop.Tests;

public class VideoCommandSinkTests
{
    internal sealed class Console(CallProtocol protocol) : IProtocolConsole
    {
        public CallProtocol Protocol => protocol;
        public long ProcessGeneration { get; private set; } = 1;
        public List<string> Commands = [];
        public event EventHandler<ProtocolConsoleLine>? LineReceived;
        public event EventHandler<ProtocolConsoleGenerationEnded>? GenerationEnded;
        public Task StartAsync(CancellationToken token) => Task.CompletedTask;
        public Task WriteLineAsync(string command, CancellationToken token) { Commands.Add(command); return Task.CompletedTask; }
        public ValueTask DisposeAsync() => ValueTask.CompletedTask;
        public void Restart() { GenerationEnded?.Invoke(this, new(ProcessGeneration)); ProcessGeneration++; }
        public void Emit(string text) => LineReceived?.Invoke(this, new ProtocolConsoleLine(text, false, ProcessGeneration));
        public void Json(int seq, string type, string fields) => LineReceived?.Invoke(this, new ProtocolConsoleLine(
            Encoding.UTF8.GetBytes($"{{\"schema\":1,\"type\":\"{type}\",\"sequence\":{seq},\"protocol\":\"{(protocol == CallProtocol.Sip ? "sip" : "h323")}\",{fields}}}\n"), false, ProcessGeneration));
        public void Ready(bool video = true) => Json(1, "media-ready", "\"sourceControl\":true,\"muteControl\":true,\"stats\":false,\"remoteSurface\":true,\"audioControl\":true,\"recording\":true,\"systemAudioControl\":true,\"roomMixerControl\":true,\"videoControl\":" + (video ? "true" : "false"));
        public void Surface(int seq, bool local) => Json(seq, local ? "local-surface" : "remote-surface", "\"hostHandle\":\"10\",\"width\":1280,\"height\":720,\"format\":\"bgra8-unorm\",\"epoch\":\"1\"");
    }
    private static async Task Connect(NativeEngineCommandSink sink, Console console)
    {
        await sink.SendAsync(new Dial("127.0.0.1", console.Protocol, "EssaStudio"), default);
        console.Emit(console.Protocol == CallProtocol.Sip ? "call 0: CONFIRMED (OK)" : "call-established remote=\"EssaStudio\""); console.Ready();
    }
    [Theory]
    [InlineData(false)] [InlineData(true)]
    public async Task Old_surface_release_cannot_remove_new_protocol_or_process_owner(bool restart)
    {
        var sip = new Console(CallProtocol.Sip); var h323 = new Console(CallProtocol.H323);
        await using var sink = new NativeEngineCommandSink(h323, sip); await sink.StartAsync(default);
        var events = new List<EngineEvent>(); sink.EventReceived += (_, e) => events.Add(e);
        await Connect(sink, sip); sip.Surface(2, false); sip.Surface(3, true);
        var oldRemote = events.OfType<RemoteSurfaceChanged>().Last().Epoch;
        var oldLocal = events.OfType<LocalSurfaceChanged>().Last().Epoch;
        sip.Emit("call 0: DISCONNECTED (local hangup)");
        if (restart) sip.Restart();
        var next = restart ? sip : h323; await Connect(sink, next); next.Surface(2, false); next.Surface(3, true);
        var remote = events.OfType<RemoteSurfaceChanged>().Last().Epoch;
        var local = events.OfType<LocalSurfaceChanged>().Last().Epoch;
        Assert.NotEqual(oldRemote, remote); Assert.NotEqual(oldLocal, local); Assert.NotEqual(remote, local);
        next.Commands.Clear();
        try { await sink.SendSurfaceAcknowledgementAsync(SurfaceAcknowledgement.Released, oldRemote, default); } catch (InvalidOperationException) { }
        try { await sink.SendLocalSurfaceAcknowledgementAsync(SurfaceAcknowledgement.Released, oldLocal, default); } catch (InvalidOperationException) { }
        Assert.Empty(next.Commands);
        await sink.SendSurfaceAcknowledgementAsync(SurfaceAcknowledgement.Opened, remote, default);
        await sink.SendLocalSurfaceAcknowledgementAsync(SurfaceAcknowledgement.Opened, local, default);
        Assert.Equal(new[] { "surface-opened 1", "local-surface-opened 1" }, next.Commands);
    }
    [Fact]
    public async Task New_commands_use_codec_and_legacy_controls_survive_missing_video_capability()
    {
        var sip = new Console(CallProtocol.Sip); var h323 = new Console(CallProtocol.H323);
        await using var sink = new NativeEngineCommandSink(h323, sip); await sink.StartAsync(default); await Connect(sink, sip);
        sip.Commands.Clear();
        await sink.SendAsync(new SetCameraEnabled(true), default); await sink.SendAsync(new SetDesktopEnabled(true, "screen"), default);
        await sink.SendAsync(new SetCameraOverlay(CameraOverlaySize.Small, CameraOverlayPosition.TopLeft), default);
        await sink.SendAsync(new SetVideoPreferences(VideoResolution.Hd, 1200), default);
        Assert.Equal(new[] { "camera on", "desktop on c2NyZWVu", "camera-overlay small top-left", "video-preferences hd 1200" }, sip.Commands);
        sip.Json(2, "media-ready", "\"sourceControl\":true,\"muteControl\":true,\"stats\":false,\"remoteSurface\":false,\"audioControl\":true,\"recording\":true,\"systemAudioControl\":true,\"roomMixerControl\":true");
        await Assert.ThrowsAsync<NotSupportedException>(() => sink.SendAsync(new SetCameraEnabled(true), default));
        await sink.SendAsync(new SwitchSource(VideoSource.Desktop, "screen"), default);
        await sink.SendAsync(new SetMicrophoneMuted(true), default); await sink.SendAsync(new SetSystemAudioEnabled(true), default);
        Assert.Equal("system-audio on", sip.Commands.Last());
    }

    [Fact]
    public async Task Explicit_pre_call_preferences_do_not_confirm_on_legacy_engine_and_legacy_toolbar_still_routes()
    {
        var sip = new Console(CallProtocol.Sip); var h323 = new Console(CallProtocol.H323);
        await using var sink = new NativeEngineCommandSink(h323, sip); await sink.StartAsync(default);
        var events=new List<EngineEvent>();sink.EventReceived+=(_,e)=>events.Add(e);
        await sink.SendAsync(new SetVideoPreferences(VideoResolution.Fhd,2500),default);
        await sink.SendAsync(new Dial("127.0.0.1",CallProtocol.Sip,"EssaStudio"),default);
        Assert.Equal("video-preferences fhd 2500",sip.Commands[0]);
        Assert.Equal("call 127.0.0.1",sip.Commands.Last());
        sip.Emit("unknown command"); sip.Emit("call 0: CONFIRMED (OK)"); sip.Ready(false);
        sip.Json(2,"recording-state","\"state\":\"idle\",\"micIncluded\":false,\"roomIncluded\":false,\"elapsedMs\":0,\"fileName\":\"\"");
        sip.Commands.Clear();
        await sink.SendAsync(new SwitchSource(VideoSource.Desktop,"screen"),default);
        await sink.SendAsync(new SetMicrophoneMuted(true),default);
        await sink.SendAsync(new SetSystemAudioEnabled(true),default);
        await sink.SendAsync(new StartLectureRecording("screen",false,false),default);
        Assert.Equal(new[]{"source desktop c2NyZWVu","mic-mute on","system-audio on","record start c2NyZWVu mic-off room-off"},sip.Commands);
        Assert.DoesNotContain(events,e=>e is VideoStateChanged);
    }

    [Fact]
    public async Task Cached_pre_call_preferences_precede_answer_without_enabling_capture()
    {
        var sip=new Console(CallProtocol.Sip);var h323=new Console(CallProtocol.H323);
        await using var sink=new NativeEngineCommandSink(h323,sip);await sink.StartAsync(default);
        await sink.SendAsync(new SetVideoPreferences(VideoResolution.Sd,null),default);
        h323.Emit("incoming-call remote=\"EssaStudio\" action=type-answer-or-reject");
        await sink.SendAsync(new Answer(),default);
        Assert.Equal(new[]{"video-preferences sd auto","answer"},h323.Commands);
    }
}
