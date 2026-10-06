using System.Collections.Concurrent;
using System.Diagnostics;
using System.IO;
using System.Text.Json;
using TeleStudio.Contracts;
using TeleStudio.Desktop;
using TeleStudio.Settings;

namespace MyStudio.WinUI;

public sealed partial class MainWindow
{
    private sealed record TransitionEvent(DateTimeOffset At, EngineEvent Value);
    private sealed record TransitionResource(string Phase, DateTimeOffset At, int Pid, bool Alive,
        int? Threads = null, int? Handles = null, long? PrivateBytes = null, long? WorkingSetBytes = null,
        double? CpuMilliseconds = null, long? StartTimeTicks = null, string? Error = null);
    private sealed class TransitionCallProof
    {
        public int Index { get; init; }
        public string Address { get; init; } = "";
        public CallProtocol Protocol { get; init; }
        public string Result { get; set; } = "RUNNING";
        public int? WorkerPid { get; set; }
        public long WorkerGeneration { get; set; }
        public bool OldWorkerExited { get; set; }
        public bool Connected { get; set; }
        public bool TerminalConfirmed { get; set; }
        public bool ControlsConfirmed { get; set; }
        public bool SameWindowRestartConfirmed { get; set; }
        public bool RecordingPausedElapsedFrozen { get; set; }
        public bool RecordingStarted { get; set; }
        public bool RecordingFinalized { get; set; }
        public string? RecordingFile { get; set; }
        public double ConnectedSeconds { get; set; }
        public List<string> Failures { get; } = [];
        public List<TransitionResource> Resources { get; } = [];
        public Dictionary<string, object> Media { get; } = [];
    }

    private async Task<bool> RunStudioTransitionSmokeIfRequestedAsync()
    {
        var args = Environment.GetCommandLineArgs();
        if (!args.Contains("--studio-transition-smoke")) return false;
        string folder;
        try { folder = Path.GetFullPath(TransitionOption(args, "--studio-transition-smoke", required: true)!); Directory.CreateDirectory(folder); }
        catch (Exception error) { WriteLog("transition/preflight", error.ToString()); await ShutdownAsync(); return true; }
        var proofs = new List<TransitionCallProof>();
        var errors = new List<string>();
        var resources = new List<TransitionResource>();
        int planned = 0;
        EventHandler<EngineEvent>? collect = null;
        Process? fixture = null;
        var jsonOptions = new JsonSerializerOptions { WriteIndented = true };
        var eventQueue = new ConcurrentQueue<TransitionEvent>();
        var evidenceOverflow = false;
        using var run = CancellationTokenSource.CreateLinkedTokenSource(lifetime.Token);
        try
        {
            var plan = StudioTransitionSmokePlan.Create(TransitionOption(args, "--transition-calls"), TransitionOption(args, "--transition-sony-address"));
            planned = plan.Calls; run.CancelAfter(TimeSpan.FromSeconds(plan.Calls * 150 + 30));
            if (Directory.EnumerateFiles(folder).Any()) throw new InvalidOperationException("Transition evidence folder must be fresh.");
            var fixturePath = TransitionExecutable(args, "--transition-share-fixture");
            var probePath = TransitionExecutable(args, "--transition-playback-probe");
            if (Environment.GetEnvironmentVariable("ESSASTUDIO_TEST_MODE") != "0") throw new InvalidOperationException("Transition proof requires actual microphone capture.");
            session.RequireIdle();
            resources.Add(SampleTransitionResource("application-initial-idle", Environment.ProcessId));
            collect = (_, value) => { if (eventQueue.Count >= 10000) evidenceOverflow = true; else eventQueue.Enqueue(new(DateTimeOffset.UtcNow, value)); };
            session.EventReceived += collect;
            var formatsByTarget = new Dictionary<string, AudioDirectionState[]>();
            var previousPid = session.Engine!.WorkerProcessId;
            var previousIdentity = previousPid is { } firstPid ? SampleTransitionResource("initial-worker", firstPid) : null;
            for (var number = 1; number <= plan.Calls; number++)
            {
                using var callLimit = CancellationTokenSource.CreateLinkedTokenSource(run.Token);
                callLimit.CancelAfter(TimeSpan.FromSeconds(150)); var token = callLimit.Token;
                var address = number % 2 == 1 ? StudioTransitionSmokePlan.CiscoAddress : plan.SonyAddress;
                var selectedProtocol = number % 2 == 1 ? CallProtocol.Sip : CallProtocol.H323;
                var proof = new TransitionCallProof { Index = number, Address = address, Protocol = selectedProtocol };
                proofs.Add(proof);
                var callFolder = Path.Combine(folder, $"call-{number:D2}-{selectedProtocol}"); Directory.CreateDirectory(callFolder);
                try
                {
                    var studio = session.SupportedStudios.Find(address) ?? throw new InvalidOperationException("Authorized studio absent from support catalog.");
                    var entry = session.Rooms.FirstOrDefault(x => x.Destination == address) ?? new(studio.Id, studio.Names[0], studio.Address, studio.EndpointProfile);
                    var priorFamily = session.Family;
                    // These are the same handlers used by the studio card and protocol UI.
                    await SelectStudioAsync(entry).WaitAsync(token);
                    await session.SelectProtocolAsync(selectedProtocol, token);
                    rendering = true; protocol.SelectedItem = selectedProtocol; rendering = false;
                    if (session.Family != studio.Family || session.Calls!.SelectedProtocol != selectedProtocol)
                        throw new InvalidOperationException("GUI selection did not apply the requested studio family/protocol.");
                    proof.WorkerPid = session.Engine!.WorkerProcessId ?? throw new InvalidOperationException("Selected GUI engine has no live worker PID.");
                    proof.WorkerGeneration = session.Engine.WorkerProcessGeneration;
                    proof.Media["requestedTuning"] = session.Tuning;
                    if (previousPid is { } oldPid && previousIdentity is not null)
                    {
                        if (priorFamily != studio.Family)
                        {
                            await WaitTransitionAsync(() => TransitionIdentityExited(previousIdentity), TimeSpan.FromSeconds(5), token);
                            if (oldPid == proof.WorkerPid && !TransitionIdentityExited(previousIdentity)) throw new InvalidOperationException("Family switch retained the previous worker.");
                            proof.OldWorkerExited = true;
                            proof.Resources.Add(SampleTransitionResource("old-worker-after-family-switch", oldPid));
                        }
                        else if (oldPid != proof.WorkerPid) throw new InvalidOperationException("Same-family selection unexpectedly replaced its worker.");
                    }
                    proof.Resources.Add(SampleTransitionResource("worker-selected-idle", proof.WorkerPid.Value));
                    proof.Resources.Add(SampleTransitionResource("application-after-selection", Environment.ProcessId));
                    previousPid = proof.WorkerPid;
                    previousIdentity = SampleTransitionResource("worker-identity", proof.WorkerPid.Value);
                    while (eventQueue.TryDequeue(out _)) { }
                    evidenceOverflow = false;
                    fixture = StartTransitionTool(fixturePath, "240");
                    var fixtureLine = await fixture.StandardOutput.ReadLineAsync(token).AsTask().WaitAsync(TimeSpan.FromSeconds(5), token);
                    using var handles = JsonDocument.Parse(fixtureLine ?? throw new IOException("Owned fixture returned no window identity."));
                    var selectedHandle = handles.RootElement.GetProperty("selectedHandle").GetInt64();
                    var snapshot = await session.Catalog!.RefreshAsync(token);
                    var source = snapshot.Items.SingleOrDefault(x => x.Kind == CaptureItemKind.Window && TransitionWindowHandle(x.Id) == selectedHandle)
                        ?? throw new InvalidOperationException("Owned fixture window absent from the actual capture catalog: " + snapshot.ErrorCode);
                    await DialAsync().WaitAsync(token);
                    await WaitTransitionAsync(() => session.Calls.IsConnected && eventQueue.Any(x => x.Value is CallConnected), TimeSpan.FromSeconds(45), token,
                        () => eventQueue.Any(x => x.Value is CallFailed or CallEnded));
                    proof.Connected = true;
                    var connected = Stopwatch.StartNew();
                    using var active = CancellationTokenSource.CreateLinkedTokenSource(token); active.CancelAfter(TimeSpan.FromSeconds(45));
                    var activeToken = active.Token;
                    proof.Resources.Add(SampleTransitionResource("worker-connected", proof.WorkerPid.Value));
                    await WaitTransitionAsync(() => callAudio is { CanToggleMicrophoneMute: true, CanToggleRoomMute: true, CanToggleSystemAudioMute: true }
                        && capabilities is { VideoControl: true, SourceControl: true, Recording: true }, TimeSpan.FromSeconds(15), activeToken);
                    await TransitionAudioToggleAsync(eventQueue, () => ToggleMicrophoneAsync(), x => x is MicrophoneMuteChanged { IsMuted: true }, activeToken);
                    await TransitionAudioToggleAsync(eventQueue, () => ToggleMicrophoneAsync(), x => x is MicrophoneMuteChanged { IsMuted: false }, activeToken);
                    await TransitionAudioToggleAsync(eventQueue, () => ToggleRoomAudioAsync(), x => x is RoomAudioMuteChanged { IsMuted: true }, activeToken);
                    await TransitionAudioToggleAsync(eventQueue, () => ToggleRoomAudioAsync(), x => x is RoomAudioMuteChanged { IsMuted: false }, activeToken);
                    await TransitionShareAsync(source, eventQueue, activeToken);
                    // Sharing can automatically enable system audio; prove an explicit
                    // OFF/ON pair through GUI handlers and fresh native acknowledgments.
                    await WaitTransitionAsync(() => callAudio!.CanToggleSystemAudioMute, TimeSpan.FromSeconds(7), activeToken);
                    if (callAudio!.ConfirmedSystemAudioEnabled)
                        await TransitionAudioToggleAsync(eventQueue, () => ToggleSystemAudioAsync(), x => x is SystemAudioChanged { Enabled: false }, activeToken);
                    await TransitionAudioToggleAsync(eventQueue, () => ToggleSystemAudioAsync(), x => x is SystemAudioChanged { Enabled: true }, activeToken);
                    proof.ControlsConfirmed = true;
                    await WaitTransitionAsync(() => CanStartSharedRecording, TimeSpan.FromSeconds(5), activeToken);
                    var recordStart = DateTimeOffset.UtcNow;
                    await StartSidebarRecordingAsync().WaitAsync(activeToken);
                    await WaitTransitionAsync(() => eventQueue.Any(x => x.At >= recordStart && x.Value is RecordingStateChanged { State: RecordingState.Recording }), TimeSpan.FromSeconds(5), activeToken);
                    proof.RecordingStarted = true;
                    await Task.Delay(2000, activeToken);
                    await SetSharedRecordingPausedAsync(true).WaitAsync(activeToken);
                    await WaitTransitionAsync(() => recording.Paused, TimeSpan.FromSeconds(5), activeToken);
                    var frozen = recording.ElapsedMs; await Task.Delay(2000, activeToken);
                    if (recording.ElapsedMs != frozen) throw new InvalidOperationException("Recording elapsed advanced while paused.");
                    proof.RecordingPausedElapsedFrozen = true;
                    await SetSharedRecordingPausedAsync(false).WaitAsync(activeToken);
                    await WaitTransitionAsync(() => !recording.Paused, TimeSpan.FromSeconds(5), activeToken);
                    for (var cycle = 0; cycle < 2; cycle++)
                    {
                        var stoppedAt = DateTimeOffset.UtcNow; await StopSharingAsync().WaitAsync(activeToken);
                        await WaitTransitionAsync(() => eventQueue.Any(x => x.At >= stoppedAt && x.Value is VideoStateChanged { DesktopRequested: false, DesktopActive: false }), TimeSpan.FromSeconds(7), activeToken);
                        await TransitionShareAsync(source, eventQueue, activeToken);
                    }
                    proof.SameWindowRestartConfirmed = true;
                    // A restarted share leaves recording paused until explicitly resumed.
                    if (recording.Paused) { await SetSharedRecordingPausedAsync(false).WaitAsync(activeToken); await WaitTransitionAsync(() => !recording.Paused, TimeSpan.FromSeconds(5), activeToken); }
                    var playbackPath = Path.Combine(callFolder, "windows-playback.json");
                    var playback = await RunTransitionProbeAsync(probePath, ["playback", proof.WorkerPid.Value.ToString(), "8", playbackPath], playbackPath, TimeSpan.FromSeconds(15), activeToken);
                    proof.Media["windowsPlayback"] = playback.Data;
                    if (!playback.Success || !TransitionNumber(playback.Data, "pid", proof.WorkerPid.Value, exact: true)
                        || !TransitionNumber(playback.Data, "nonzeroSamples", 1000) || !TransitionNumber(playback.Data, "peak", 100))
                        proof.Failures.Add("Fresh Windows process playback/nonzero room audio proof failed.");
                    proof.Resources.Add(SampleTransitionResource("worker-active-media", proof.WorkerPid.Value));
                    proof.Resources.Add(SampleTransitionResource("application-active-media", Environment.ProcessId));
                    var lastSample = connected.Elapsed;
                    while (connected.Elapsed < TimeSpan.FromSeconds(45))
                    {
                        // Check active ownership on each dwell step, including the final step.
                        if (!session.Calls.IsConnected || eventQueue.Any(x => x.Value is CallFailed or CallEnded)) throw new InvalidOperationException("Call ended during its 45-second stability window.");
                        if (eventQueue.Any(x => x.Value is MediaError { Code: "capture-device-failed" })) throw new InvalidOperationException("Real microphone capture device failed.");
                        if (evidenceOverflow) throw new InvalidOperationException("Per-call event evidence exceeded its bounded capacity.");
                        if (connected.Elapsed - lastSample > TimeSpan.FromSeconds(5))
                        {
                            proof.Resources.Add(SampleTransitionResource("worker-stable-dwell", proof.WorkerPid.Value));
                            proof.Resources.Add(SampleTransitionResource("application-stable-dwell", Environment.ProcessId)); lastSample = connected.Elapsed;
                        }
                        await Task.Delay(TimeSpan.FromMilliseconds(Math.Min(100, Math.Max(1, (TimeSpan.FromSeconds(45) - connected.Elapsed).TotalMilliseconds))), token);
                    }
                    proof.ConnectedSeconds = connected.Elapsed.TotalSeconds;
                    if (!session.Calls.IsConnected) throw new InvalidOperationException("Call no longer connected at the final stability boundary.");
                    VerifyTransitionMedia(proof, eventQueue.ToArray(), formatsByTarget);
                    proof.Media["actualVideoState"] = eventQueue.Select(x => x.Value).OfType<VideoStateChanged>().LastOrDefault()!;
                    await SaveCaptureAsync(Path.Combine(callFolder, "actual-gui-call.png")).WaitAsync(token);
                    proof.Resources.Add(SampleTransitionResource("worker-before-hangup", proof.WorkerPid.Value));
                    await TransitionHangupAsync(proof, eventQueue);
                    await WaitTransitionAsync(() => TransitionCompletedRecording(eventQueue) is not null, OpalCallLifecycle.TerminationTimeout, CancellationToken.None);
                    var completedRecording = TransitionCompletedRecording(eventQueue)!;
                    if (completedRecording.State != RecordingState.Completed) throw new InvalidOperationException("Recording finalization failed during hangup.");
                    proof.RecordingFinalized = true;
                    proof.RecordingFile = Path.Combine(RecordingLocationStore.ForCurrentUser().Load() ?? Path.Combine(AppDataPaths.Root, "Recordings"), completedRecording.FileName);
                    var decodePath = Path.Combine(callFolder, "recording-decode.json");
                    var decode = await RunTransitionProbeAsync(probePath, ["recording-pattern", proof.RecordingFile, decodePath], decodePath, TimeSpan.FromSeconds(30), token);
                    proof.Media["recordingDecode"] = decode.Data;
                    if (!decode.Success || !TransitionNumber(decode.Data, "videoWidth", 1920, exact: true) || !TransitionNumber(decode.Data, "videoHeight", 1080, exact: true)
                        || !TransitionNumber(decode.Data, "videoSamples", 10) || !TransitionNumber(decode.Data, "audioSamples", 10)
                        || !TransitionNumber(decode.Data, "audioNonzeroPcmSamples", 1000)) proof.Failures.Add("Finalized owned-window MP4 decode/pattern/audio proof failed.");
                    await Task.Delay(2000, token);
                    proof.Resources.Add(SampleTransitionResource("worker-settled-idle-after-hangup", proof.WorkerPid.Value));
                    proof.Resources.Add(SampleTransitionResource("application-settled-idle-after-hangup", Environment.ProcessId));
                    proof.Result = proof.Failures.Count == 0 ? "PASS" : "MEDIA_FAIL";
                }
                catch (Exception error)
                {
                    proof.Result = "FAIL"; proof.Failures.Add(error.ToString());
                    throw; // Command, timeout or cleanup failure stops further physical calls.
                }
                finally
                {
                    // Finish this call while its owned source window still exists.
                    // A failed phase never borrows the cancelled test deadline here.
                    if (!proof.TerminalConfirmed && session.Calls?.CanHangUp == true)
                    {
                        try { await TransitionHangupAsync(proof, eventQueue); }
                        catch (Exception error) { proof.Failures.Add("Call cleanup: " + error); proof.Result = "FAIL"; errors.Add("Call cleanup failed."); }
                    }
                    if (proof.RecordingStarted && !proof.RecordingFinalized)
                    {
                        try
                        {
                            await WaitTransitionAsync(() => TransitionCompletedRecording(eventQueue) is not null,
                                OpalCallLifecycle.TerminationTimeout, CancellationToken.None);
                            var completed = TransitionCompletedRecording(eventQueue)!;
                            proof.RecordingFinalized = completed.State == RecordingState.Completed;
                            if (!proof.RecordingFinalized) throw new IOException("Recording failed during cleanup.");
                            proof.RecordingFile ??= Path.Combine(RecordingLocationStore.ForCurrentUser().Load() ?? Path.Combine(AppDataPaths.Root, "Recordings"), completed.FileName);
                        }
                        catch (Exception error) { proof.Failures.Add("Recording cleanup: " + error); errors.Add("Recording cleanup failed."); proof.Result = "FAIL"; }
                    }
                    if (fixture is not null) { try { await StopTransitionFixtureAsync(fixture); } catch (Exception error) { proof.Failures.Add("Fixture cleanup: " + error); proof.Result = "FAIL"; errors.Add("Owned fixture cleanup failed."); } fixture.Dispose(); fixture = null; }
                    File.WriteAllText(Path.Combine(callFolder, "events.json"), JsonSerializer.Serialize(eventQueue.Select(x => new { x.At, Type = x.Value.GetType().Name, Data = JsonSerializer.SerializeToElement(x.Value, x.Value.GetType()) }), jsonOptions));
                    File.WriteAllText(Path.Combine(callFolder, "proof.json"), JsonSerializer.Serialize(proof, jsonOptions));
                    WriteTransitionReport();
                }
                if (errors.Count != 0) break;
            }
        }
        catch (Exception error) { errors.Add(error.ToString()); }
        finally
        {
            // Cleanup has its own budget even when the test deadline/caller was cancelled.
            var current = proofs.LastOrDefault();
            try { if (session.Calls?.CanHangUp == true && current is not null) await TransitionHangupAsync(current, eventQueue); }
            catch (Exception error) { errors.Add("Final hangup: " + error); }
            var lastWorker = session.Engine?.WorkerProcessId;
            await ShutdownWithEvidenceAsync(errors);
            if (lastWorker is { } pid)
            {
                resources.Add(SampleTransitionResource("final-worker-after-disposal", pid));
                if (resources[^1].Alive) errors.Add("Final owned worker survived GUI session disposal.");
            }
            resources.Add(SampleTransitionResource("application-final-idle", Environment.ProcessId));
            if (collect is not null) session.EventReceived -= collect;
            try { TeleStudio.Desktop.Runtime.DiagnosticLog.ExportSnapshot(Path.Combine(folder, "application-diagnostics.txt")); }
            catch (Exception error) { errors.Add("Diagnostics export: " + error); }
            WriteTransitionReport();
        }
        return true;

        void WriteTransitionReport() => File.WriteAllText(Path.Combine(folder, "studio-transition-smoke.json"), JsonSerializer.Serialize(new {
            result = errors.Count == 0 && proofs.Count == planned && proofs.All(x => x.Result == "PASS") ? "PASS" : "FAIL",
            frontend = "Microsoft.UI.Xaml", actualGuiWorkflow = true, syntheticMicrophone = false, plannedCalls = planned,
            completedCalls = proofs.Count(x => x.TerminalConfirmed), calls = proofs, resources, errors,
            limitation = "Local playback/capture and resource samples do not prove remote physical audibility/display or absence of long-term leaks."
        }, jsonOptions));
    }

    private async Task TransitionAudioToggleAsync(ConcurrentQueue<TransitionEvent> events, Func<Task> action, Func<EngineEvent, bool> confirmation, CancellationToken token)
    {
        var requested = DateTimeOffset.UtcNow; await action().WaitAsync(token);
        await WaitTransitionAsync(() => events.Any(x => x.At >= requested && confirmation(x.Value)), TimeSpan.FromSeconds(7), token);
    }
    private static RecordingStateChanged? TransitionCompletedRecording(ConcurrentQueue<TransitionEvent> events) =>
        events.Select(x => x.Value).OfType<RecordingStateChanged>().LastOrDefault(x => x.State is RecordingState.Completed or RecordingState.Failed);
    private async Task TransitionShareAsync(CaptureCatalogItem source, ConcurrentQueue<TransitionEvent> events, CancellationToken token)
    {
        var requested = DateTimeOffset.UtcNow; await SelectSharingSourceAsync(source).WaitAsync(token);
        await WaitTransitionAsync(() => events.Any(x => x.At >= requested && x.Value is VideoStateChanged { DesktopRequested: true, DesktopActive: true, DesktopError: "" })
            && sharedSource?.Id == source.Id, TimeSpan.FromSeconds(10), token);
    }
    private async Task TransitionHangupAsync(TransitionCallProof proof, ConcurrentQueue<TransitionEvent> events)
    {
        using var cleanup = new CancellationTokenSource(OpalCallLifecycle.TerminationTimeout);
        var requested = DateTimeOffset.UtcNow;
        await (session.Calls?.HangUpAsync(cleanup.Token) ?? Task.CompletedTask).WaitAsync(cleanup.Token);
        await WaitTransitionAsync(() => events.Any(x => x.At >= requested && x.Value is CallEnded) && session.Calls?.CanHangUp != true,
            OpalCallLifecycle.TerminationTimeout, cleanup.Token);
        proof.TerminalConfirmed = true;
    }
    private static async Task WaitTransitionAsync(Func<bool> predicate, TimeSpan bound, CancellationToken token, Func<bool>? failure = null)
    {
        using var limit = CancellationTokenSource.CreateLinkedTokenSource(token); limit.CancelAfter(bound);
        try { while (!predicate()) { if (failure?.Invoke() == true) throw new InvalidOperationException("Native call failed before confirmation."); await Task.Delay(50, limit.Token); } }
        catch (OperationCanceledException) when (!token.IsCancellationRequested) { throw new TimeoutException("GUI transition phase did not receive its required fresh confirmation within " + bound); }
    }
    private static void VerifyTransitionMedia(TransitionCallProof proof, TransitionEvent[] events, Dictionary<string, AudioDirectionState[]> baselines)
    {
        var health = events.Where(x => x.Value is SipConnectionHealthChanged).ToArray();
        var latest = health.LastOrDefault(); var earlier = latest is null ? null : health.LastOrDefault(x => x.At <= latest.At.AddSeconds(-8));
        if (latest?.Value is not SipConnectionHealthChanged final || earlier?.Value is not SipConnectionHealthChanged initial)
            proof.Failures.Add("Fresh health samples spanning the final stability window are missing.");
        else
        {
            var delta = new { captured = final.CapturedSamples - initial.CapturedSamples, decodedNonzero = final.DecodedNonzeroSamples - initial.DecodedNonzeroSamples,
                submittedNonzero = final.DeviceSubmittedNonzeroSamples - initial.DeviceSubmittedNonzeroSamples, sentFrames = final.SentFrames - initial.SentFrames, receivedFrames = final.ReceivedFrames - initial.ReceivedFrames };
            proof.Media["finalHealth"] = final; proof.Media["finalWindowDelta"] = delta;
            if (delta.captured < 1000 || delta.decodedNonzero < 1000 || delta.submittedNonzero < 1000 || delta.sentFrames < 5)
                proof.Failures.Add("Real microphone/nonzero decoded audio/device-submitted audio/outgoing frames did not advance in the final window.");
        }
        // The managed engine publishes negotiated directions only after connection,
        // including buffered pre-connected stream snapshots. They are not periodic.
        var directions = events.Where(x => x.Value is AudioDirectionState).Select(x => (AudioDirectionState)x.Value).ToArray();
        var current = directions.GroupBy(x => x.Direction).Select(x => x.First()).OrderBy(x => x.Direction).ToArray();
        proof.Media["audioDirections"] = directions;
        if (current.Length != 2 || current.Any(x => x.SampleRate <= 0 || x.Channels <= 0 || x.BitrateKbps <= 0 || x.Codec.Contains("711", StringComparison.OrdinalIgnoreCase)))
            proof.Failures.Add("Two valid negotiated send/receive audio directions are missing or use excluded G.711.");
        if (directions.Any(x => current.First(y => y.Direction == x.Direction) != x)) proof.Failures.Add("Negotiated audio format changed during the settled call window.");
        var key = proof.Address + "/" + proof.Protocol;
        if (baselines.TryGetValue(key, out var baseline) && !baseline.SequenceEqual(current)) proof.Failures.Add("Negotiated audio formats changed across repeated dialogs for the same target/protocol.");
        else if (current.Length == 2) baselines[key] = current;
    }
    private static string? TransitionOption(string[] args, string name, bool required = false)
    {
        var i = Array.IndexOf(args, name); if (i < 0) { if (required) throw new ArgumentException(name + " is required."); return null; }
        if (i + 1 >= args.Length || args[i + 1].StartsWith("--", StringComparison.Ordinal)) throw new ArgumentException(name + " requires a value.");
        return args[i + 1];
    }
    private static string TransitionExecutable(string[] args, string name)
    {
        var path = Path.GetFullPath(TransitionOption(args, name, true)!);
        if (!File.Exists(path) || !string.Equals(Path.GetExtension(path), ".exe", StringComparison.OrdinalIgnoreCase)) throw new FileNotFoundException("Owned proof executable is missing.", path);
        return path;
    }
    private static Process StartTransitionTool(string path, params string[] arguments)
    {
        var start = new ProcessStartInfo(path) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardInput = true, RedirectStandardOutput = true, RedirectStandardError = true };
        foreach (var value in arguments) start.ArgumentList.Add(value);
        return Process.Start(start) ?? throw new IOException("Owned proof helper did not start.");
    }
    private static async Task<(bool Success, JsonElement Data)> RunTransitionProbeAsync(string path, string[] arguments, string proofPath, TimeSpan bound, CancellationToken token)
    {
        if (File.Exists(proofPath)) throw new IOException("Probe output exists; fresh evidence is required.");
        using var probe = StartTransitionTool(path, arguments);
        var stdout = probe.StandardOutput.ReadToEndAsync(); var stderr = probe.StandardError.ReadToEndAsync();
        try { await probe.WaitForExitAsync(token).WaitAsync(bound, token); }
        catch { try { if (!probe.HasExited) probe.Kill(entireProcessTree: true); } finally { await probe.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(3)); } throw; }
        await Task.WhenAll(stdout, stderr).WaitAsync(TimeSpan.FromSeconds(2), token);
        File.WriteAllText(proofPath + ".log", await stdout + Environment.NewLine + await stderr);
        if (!File.Exists(proofPath)) return (false, JsonSerializer.SerializeToElement(new { error = "Fresh probe report missing", probe.ExitCode }));
        using var data = JsonDocument.Parse(File.ReadAllText(proofPath)); var result = data.RootElement.Clone();
        return (probe.ExitCode == 0 && result.TryGetProperty("result", out var state) && state.GetString() == "PASS", result);
    }
    private static async Task StopTransitionFixtureAsync(Process fixture)
    {
        if (fixture.HasExited) throw new IOException("Owned sharing fixture exited before controlled cleanup.");
        using var cleanup = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        try { await fixture.StandardInput.WriteLineAsync("quit".AsMemory(), cleanup.Token).WaitAsync(cleanup.Token); await fixture.StandardInput.FlushAsync(cleanup.Token); await fixture.WaitForExitAsync(cleanup.Token); }
        catch { if (!fixture.HasExited) { fixture.Kill(entireProcessTree: true); await fixture.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(3)); } throw; }
        if (fixture.ExitCode != 0) throw new IOException("Owned sharing fixture exited with code " + fixture.ExitCode);
    }
    private static bool TransitionNumber(JsonElement data, string name, long threshold, bool exact = false) =>
        data.TryGetProperty(name, out var value) && value.TryGetInt64(out var number) && (exact ? number == threshold : number > threshold);
    private static long? TransitionWindowHandle(string source)
    {
        if (!source.StartsWith("esw1.", StringComparison.Ordinal)) return null;
        try { var encoded = source[5..].Replace('-', '+').Replace('_', '/'); var bytes = Convert.FromBase64String(encoded.PadRight((encoded.Length + 3) / 4 * 4, '=')); return bytes.Length >= 12 ? BitConverter.ToInt64(bytes, 4) : null; }
        catch (FormatException) { return null; }
    }
    private static TransitionResource SampleTransitionResource(string phase, int pid)
    {
        try { using var process = Process.GetProcessById(pid); process.Refresh(); if (process.HasExited) return new(phase, DateTimeOffset.UtcNow, pid, false);
            return new(phase, DateTimeOffset.UtcNow, pid, true, process.Threads.Count, process.HandleCount, process.PrivateMemorySize64, process.WorkingSet64, process.TotalProcessorTime.TotalMilliseconds, process.StartTime.ToUniversalTime().Ticks); }
        catch (ArgumentException) { return new(phase, DateTimeOffset.UtcNow, pid, false); }
        catch (InvalidOperationException) { return new(phase, DateTimeOffset.UtcNow, pid, false); }
        catch (Exception error) { return new(phase, DateTimeOffset.UtcNow, pid, true, Error: error.Message); }
    }
    private static bool TransitionIdentityExited(TransitionResource identity)
    {
        var current = SampleTransitionResource("identity-check", identity.Pid);
        return !current.Alive || (identity.StartTimeTicks is { } old && current.StartTimeTicks is { } now && old != now);
    }
}
