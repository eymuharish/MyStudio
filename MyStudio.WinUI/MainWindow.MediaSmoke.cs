using System.IO;
using System.Text.Json;
using TeleStudio.Contracts;
using TeleStudio.Desktop;
using TeleStudio.Settings;

namespace MyStudio.WinUI;
public sealed partial class MainWindow
{
    private async Task RunLocalMediaSmokeIfRequestedAsync()
    {
        var args = Environment.GetCommandLineArgs(); var index = Array.IndexOf(args, "--local-media-smoke");
        if (args.Contains("--call-bar-smoke")) return;
        if (index < 0 || index + 1 >= args.Length) return;
        var folder = Path.GetFullPath(args[index + 1]); Directory.CreateDirectory(folder);
        var native = Path.Combine(OpalEngineCommandSink.ResolveOpalDirectory(), "opal_console.exe");
        var peerLaunch = MediaSmokePeerLaunch.Create(native, "--sip-port 5141 --h323-port 1761 --rtp-port 43200 --interface 127.0.0.1", args);
        using var limit = new CancellationTokenSource(TimeSpan.FromSeconds(140)); var token = limit.Token;
        var right = new ProtocolConsoleProcess(CallProtocol.Sip, peerLaunch.Executable, peerLaunch.Arguments, "Listening SIP on ",
            () => new Dictionary<string, string> { ["ESSASTUDIO_TEST_MODE"] = "1", ["ESSASTUDIO_TEST_TONE_HZ"] = "880" }, ["Listening H.323 on "], sharedProtocols: true, shutdownTimeout: OpalCallLifecycle.TerminationTimeout);
        var receiver = new OpalEngineCommandSink(new SharedOpalConsole(right), EngineFamily.Cisco);
        bool incoming = false; SipConnectionHealthChanged? health = null;
        receiver.EventReceived += (_, value) => { if (value is IncomingCall) incoming = true; if (value is SipConnectionHealthChanged next) health = next; };
        var logGate = new object();
        right.LineReceived += (_, line) => { lock (logGate) File.AppendAllText(Path.Combine(folder, "peer-native.log"), System.Text.Encoding.UTF8.GetString(line.Bytes.Span) + Environment.NewLine); };
        var hashes = new HashSet<ulong>();
        try
        {
            session.RequireIdle(); await receiver.StartAsync(token);
            rendering = true; backend.SelectedItem = SipEngineKind.Opal; endpoint.SelectedItem = EndpointProfile.Ex90; destination.Text = "127.0.0.1:5141"; rendering = false;
            await DialAsync(); await WaitAsync(() => incoming, token); await receiver.SendAsync(new Answer(), token);
            await WaitAsync(() => session.Calls!.IsConnected && capabilities?.VideoControl == true, token);
            await WaitAsync(() => session.Engine!.EchoState.Available && session.Engine.EchoState.Enabled, token);
            await SendAsync(new SetEchoCancellation(false)); await WaitAsync(() => !session.Engine!.EchoState.Enabled, token);
            await SendAsync(new SetEchoCancellation(true)); await WaitAsync(() => session.Engine!.EchoState.Enabled, token);
            for (var i = 0; i < 120; i++) { await Task.Delay(100, token); if (remotePresenter.LastFingerprint != 0) hashes.Add(remotePresenter.LastFingerprint); }
            if (remotePresenter.PresentedFrames < 20 || hashes.Count < 3 || (health?.ReceivedFrames ?? 0) < 20)
                throw new InvalidOperationException($"Actual WinUI presentation/outbound media proof failed: remote={remotePresenter.PresentedFrames}, moving={hashes.Count}, peer decoded={health?.ReceivedFrames}, presenter error={remotePresenter.Error}");
            await SaveCaptureAsync(Path.Combine(folder, "actual-winui-call.png"));
            await RunSidebarRecordingProofAsync(folder, token);
            if (args.Contains("--room-audio-controls-check"))
            {
                await SendAsync(new SetRoomMixer(300, 0, 0, 0));
                await WaitAsync(() => confirmedMixer.MasterPercent == 300, token);
            }
            dialogCaptureFolder = folder;
            try
            {
                await ShowAudioAsync();
                if (args.Contains("--room-audio-controls-check"))
                {
                    await WaitAsync(() => confirmedMixer is { MasterPercent: 300, BassDb: 0, VoiceDb: 6, TrebleDb: 0, NoiseReductionDb: 0 }, token);
                    if (confirmedAudio.MicrophoneGain != 100 || !confirmedAudio.NoiseIsolation)
                        throw new InvalidOperationException("Incoming room presets changed the microphone signal controls.");
                }
                await ShowVideoAsync(); await ShowRecordingAsync();
            }
            finally { dialogCaptureFolder = null; }
            File.WriteAllText(Path.Combine(folder, "winui-media-smoke.json"), JsonSerializer.Serialize(new { result = "PASS", frontend = "Microsoft.UI.Xaml", mode = remotePresenter.Mode, presentedFrames = remotePresenter.PresentedFrames, distinctFingerprints = hashes.Count, peerDecodedVideoFrames = health!.ReceivedFrames, peerDecodedAudioSamples = health.DecodedAudioSamples, audioEvidence, localFrames = localPresenter.PresentedFrames, cpuCopyMilliseconds = remotePresenter.LastCopyMilliseconds,
                echoCancellation = session.Engine!.EchoState, echoToggleConfirmed = true,
                roomAudioControlsChecked = args.Contains("--room-audio-controls-check"), confirmedIncomingRoomMixer = confirmedMixer,
                sharingTransmissionUiChecked = args.Contains("--sharing-transmission-check"),
                peerDebugger = peerLaunch.Executable != native ? peerLaunch.Executable : null, peerStacksAfterMs = peerLaunch.Executable != native ? (int?)35000 : null }, new JsonSerializerOptions { WriteIndented = true }));
        }
        catch (Exception error) { File.WriteAllText(Path.Combine(folder, "winui-media-smoke-error.txt"), error.ToString()); }
        finally
        {
            var cleanupErrors = await SmokeCleanup.RunAsync(new Func<Task>[] {
                () => session.Calls?.HangUpAsync(CancellationToken.None) ?? Task.CompletedTask,
                () => receiver.DisposeAsync().AsTask()
            }, args.Contains("--ui-smoke") ? null : ShutdownAsync);
            if (cleanupErrors.Count > 0)
                try { File.AppendAllText(Path.Combine(folder, "winui-media-smoke-error.txt"), "\nCleanup: " + string.Join("\n", cleanupErrors)); }
                catch { /* Continue into the UI smoke's final shutdown even if its evidence folder is unavailable. */ }
        }
    }
    private static async Task WaitAsync(Func<bool> predicate, CancellationToken token) { while (!predicate()) await Task.Delay(50, token); }
}
