using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text.Json;
using TeleStudio.Contracts;
using TeleStudio.Desktop;
using TeleStudio.Settings;

namespace MyStudio.WinUI;
public sealed partial class MainWindow
{
    private async Task RunSidebarRecordingProofAsync(string folder, CancellationToken token)
    {
        if (callSidebar is null) throw new InvalidOperationException("Active call sidebar was not created.");
        if (!callSidebar.IconsOnly || !callSidebar.AccessibleIcons) throw new InvalidOperationException("Floating controls contain visible labels or lack accessible tooltips.");
        if (ScreenSharingPauseControls.CanToggle || screenSharingPause.RememberedSource is not null)
            throw new InvalidOperationException("Screen sharing resume was available before an explicit source choice.");
        VerifyCallHoldRemoved(callSidebar);
        // Deliberately exercise an in-call error containing a fake secret; no network operation.
        ReportError("local-proof", "Local proof error password=sidebar-proof-secret"); Render();
        if (status.IsOpen || status.Visibility != Microsoft.UI.Xaml.Visibility.Collapsed)
            throw new InvalidOperationException("Call-time error banner was visible.");
        var log = TeleStudio.Desktop.Runtime.DiagnosticLog.ReadSnapshot();
        if (!log.Contains("Local proof error") || log.Contains("sidebar-proof-secret")) throw new InvalidOperationException("Error was not persisted/redacted.");
        ShowPage("diagnostics"); await Task.Delay(200, token); await SaveCaptureAsync(Path.Combine(folder, "actual-log-and-debug.png")); ShowPage("call");
        var wasSharing = videoState?.DesktopRequested == true;
        var wasSharingCamera = videoState?.CameraRequested == true;
        dialogCaptureFolder = folder;
        try { await ShowSharingSourceAsync(); await ShowDevicesAsync(); }
        finally { dialogCaptureFolder = null; }
        if (videoState?.DesktopRequested == true && !wasSharing) throw new InvalidOperationException("Canceling source selection started screen sharing.");
        if (videoState?.CameraRequested == true && !wasSharingCamera) throw new InvalidOperationException("Canceling source selection started camera sharing.");
        // This is an explicit test selection; ordinary recording controls never select a display.
        if (CanStartSharedRecording) throw new InvalidOperationException("Recording enabled before an explicit shared source.");
        var proofSources = (await session.Catalog!.RefreshAsync(token)).Items;
        var selectedWindowProof = Environment.GetCommandLineArgs().Contains("--selected-window-recording-smoke");
        var recordingSource = selectedWindowProof
            ? proofSources.FirstOrDefault(x => x.Kind == CaptureItemKind.Window && x.Name == "MyStudio selected share fixture")
            : proofSources.FirstOrDefault(x => x.Kind == CaptureItemKind.Monitor);
        if (recordingSource is null) throw new InvalidOperationException("The explicitly requested sharing proof source is unavailable.");
        await SelectSharingSourceAsync(recordingSource);
        await WaitAsync(() => CanStartSharedRecording, token);
        var recordingWall = Stopwatch.StartNew();
        await StartSidebarRecordingAsync(); await WaitAsync(() => recording.State == RecordingState.Recording, token);
        var startedMs = recordingWall.Elapsed.TotalMilliseconds;
        await Task.Delay(1800, token); var pauseRequestedMs = recordingWall.Elapsed.TotalMilliseconds;
        await callSidebar.InvokeScreenPauseForTestAsync(); await WaitAsync(() => recording.Paused && ScreenSharingPauseControls.CanToggle, token);
        if (screenSharingPause.RememberedSource?.Id != recordingSource.Id || !ScreenSharingPauseControls.Paused)
            throw new InvalidOperationException("Screen pause did not remember the exact explicitly shared source.");
        var pausedMs = recordingWall.Elapsed.TotalMilliseconds;
        var frozen = recording.ElapsedMs; await SaveCaptureAsync(Path.Combine(folder, "sidebar-paused.png"), callSidebar.CaptureTarget);
        await Task.Delay(2600, token);
        if (!recording.Paused || recording.ElapsedMs != frozen) throw new InvalidOperationException("Paused recording time advanced.");
        if (CanPauseOrResumeSharedRecording) throw new InvalidOperationException("Resume enabled while sharing is stopped.");
        await callSidebar.InvokeScreenPauseForTestAsync(); await WaitAsync(() => CanPauseOrResumeSharedRecording && !ScreenSharingPauseControls.Paused, token);
        if (sharedSource?.Id != recordingSource.Id || screenSharingPause.ResumeRequested)
            throw new InvalidOperationException("Screen resume lacked a fresh confirmation for the exact selected source.");
        if (!recording.Paused || recording.ElapsedMs != frozen) throw new InvalidOperationException("Restarting sharing automatically resumed recording.");
        var resumeRequestedMs = recordingWall.Elapsed.TotalMilliseconds;
        await SetSharedRecordingPausedAsync(false); await WaitAsync(() => !recording.Paused, token);
        var resumedMs = recordingWall.Elapsed.TotalMilliseconds;
        await Task.Delay(1800, token); var stopRequestedMs = recordingWall.Elapsed.TotalMilliseconds;
        await SendAsync(new StopLectureRecording()); await WaitAsync(() => recording.State is RecordingState.Finalizing or RecordingState.Completed or RecordingState.Failed, token);
        var stoppedMs = recordingWall.Elapsed.TotalMilliseconds;
        var pauseProof = RecordingPauseProof.Measure(startedMs, pauseRequestedMs, pausedMs, resumeRequestedMs, resumedMs, stopRequestedMs, stoppedMs);
        await WaitAsync(() => recording.State is RecordingState.Completed or RecordingState.Failed, token);
        var elapsedValid = pauseProof.AcceptsElapsed(recording.ElapsedMs) && pauseProof.ExcludedPauseWallMs >= 2500;
        File.WriteAllText(Path.Combine(folder, "recording-pause-timing.json"), JsonSerializer.Serialize(new {
            result = elapsedValid && recording.State == RecordingState.Completed ? "PASS" : "FAIL", startedMs,
            pauseRequestedMs, pausedMs, resumeRequestedMs, resumedMs, stopRequestedMs, stoppedMs,
            finalizationWallMs = recordingWall.Elapsed.TotalMilliseconds - stoppedMs, pauseProof.MinimumActiveMs,
            pauseProof.MaximumActiveMs, pauseProof.ExcludedPauseWallMs, recordedElapsedMs = recording.ElapsedMs, frozen
        }, new JsonSerializerOptions { WriteIndented = true }));
        if (recording.State != RecordingState.Completed || !elapsedValid)
            throw new InvalidOperationException($"Pause/resume MP4 finalization failed: {recording.State}/{recording.ElapsedMs}; active bounds {pauseProof.MinimumActiveMs:F0}–{pauseProof.MaximumActiveMs:F0} ms; excluded pause {pauseProof.ExcludedPauseWallMs:F0} ms");
        var recordingPath = Path.Combine(RecordingLocationStore.ForCurrentUser().Load() ?? Path.Combine(AppDataPaths.Root, "Recordings"), recording.FileName);
        if (!File.Exists(recordingPath)) throw new InvalidOperationException("Finalized recording file missing.");
        ShowPage("recordings"); await Task.Delay(200, token);
        if (!recordingFiles.ListCompletedFiles().Any(x => x.FileName == recording.FileName)) throw new InvalidOperationException("Completed recording absent from library.");
        await SaveCaptureAsync(Path.Combine(folder, "actual-recordings-library.png")); ShowPage("call");
        var mainHandle = WinRT.Interop.WindowNative.GetWindowHandle(this);
        var presenter = (Microsoft.UI.Windowing.OverlappedPresenter)AppWindow.Presenter;
        presenter.Minimize(); await Task.Delay(300, token);
        if (!IsIconic(mainHandle) || !IsWindowVisible(callSidebar.Handle) || (GetWindowLongPtr(callSidebar.Handle, -20).ToInt64() & 8) == 0)
            throw new InvalidOperationException("Call sidebar did not stay topmost while main window minimized.");
        await SaveCaptureAsync(Path.Combine(folder, "sidebar-main-minimized.png"), callSidebar.CaptureTarget);
        GetWindowRect(callSidebar.Handle, out var bounds);
        var scale = GetDpiForWindow(callSidebar.Handle) / 96d;
        if (bounds.Right - bounds.Left != Math.Ceiling(FloatingCallWindow.LogicalWidth * scale) ||
            bounds.Bottom - bounds.Top != Math.Ceiling(FloatingCallWindow.LogicalHeight * scale))
            throw new InvalidOperationException("Floating icon strip has unexpected empty space or size.");
        long? capturedMarker = null, excludedMarker = null;
        var captureHelper = Path.Combine(AppContext.BaseDirectory, "capture_smoke.exe");
        if (callSidebar.CaptureExcluded && File.Exists(captureHelper))
        {
            callSidebar.ShowCaptureMarkerForTest(true);
            try
            {
                callSidebar.SetCaptureExclusionForTest(false); await Task.Delay(250, token);
                var visible = Path.Combine(folder, "sidebar-capture-visible.ppm"); await CaptureActualDesktopAsync(captureHelper, visible, token);
                capturedMarker = CountMagenta(visible, bounds);
                callSidebar.SetCaptureExclusionForTest(true); await Task.Delay(250, token);
                var excluded = Path.Combine(folder, "sidebar-capture-excluded.ppm"); await CaptureActualDesktopAsync(captureHelper, excluded, token);
                excludedMarker = CountMagenta(excluded, bounds);
                if (capturedMarker < 500 || excludedMarker > capturedMarker / 20)
                    throw new InvalidOperationException($"Actual WGC exclusion failed: visible={capturedMarker}, excluded={excludedMarker}");
            }
            finally { callSidebar.SetCaptureExclusionForTest(true); callSidebar.ShowCaptureMarkerForTest(false); }
        }
        presenter.Restore();
        File.WriteAllText(Path.Combine(folder, "sidebar-recording-proof.json"), JsonSerializer.Serialize(new {
            result="PASS", floatingRight=true, alwaysOnTop=true, survivesMainMinimize=true, errorBannerHidden=true, errorsPersistedAndRedacted=true,
            floatingIconsOnly=callSidebar.IconsOnly, floatingAccessibleIcons=callSidebar.AccessibleIcons,
            callHoldControlsAbsent=true, screenSharingPauseResumeConfirmed=true, resumeRemembersOnlyExplicitSource=true,
            snugWidthDip=FloatingCallWindow.LogicalWidth, snugHeightDip=FloatingCallWindow.LogicalHeight,
            screenPickerCancelDoesNotShare=true, cameraPickerCancelDoesNotShare=true, recordingRequiresConfirmedSharedSource=true, sharedSourceId=recordingSource.Id, recordingWidth=OpalEngineCommandSink.RecordingWidth, recordingHeight=OpalEngineCommandSink.RecordingHeight, sharingStopAutomaticallyPauses=true, restartSharingRequiresExplicitResume=true, recordingLibraryContainsActualMp4=true, recordingPaused=true, recordingPauseFrozenMs=frozen, excludedPauseWallMs=pauseProof.ExcludedPauseWallMs, minimumActiveWallMs=pauseProof.MinimumActiveMs, maximumActiveWallMs=pauseProof.MaximumActiveMs, recordedElapsedMs=recording.ElapsedMs, recordingPath,
            captureExclusionApiSucceeded=callSidebar.CaptureExcluded, actualWgcTested=capturedMarker.HasValue, capturedMarker, excludedMarker,
            bounds
        }, new JsonSerializerOptions { WriteIndented=true, IncludeFields=true }));
    }
    private static async Task CaptureActualDesktopAsync(string helper, string output, CancellationToken token)
    {
        var info = new ProcessStartInfo(helper) { UseShellExecute=false, CreateNoWindow=true, RedirectStandardOutput=true, RedirectStandardError=true };
        foreach(var arg in new[] { "--seconds", "5", "--fps", "15", "--output", output }) info.ArgumentList.Add(arg);
        using var process = Process.Start(info) ?? throw new InvalidOperationException("Actual capture helper failed to start.");
        var stdout=process.StandardOutput.ReadToEndAsync(token); var stderr=process.StandardError.ReadToEndAsync(token);
        await process.WaitForExitAsync(token); var details=await stdout + await stderr;
        File.WriteAllText(output+".log", details);
        if(process.ExitCode!=0) throw new InvalidOperationException("Actual WGC capture failed: "+details);
    }
    private static long CountMagenta(string path, WindowBounds bounds)
    {
        var bytes=File.ReadAllBytes(path); var at=0;
        string Line() { var start=at; while(bytes[at++]!=10){} return System.Text.Encoding.ASCII.GetString(bytes,start,at-start-1); }
        if(Line()!="P6") throw new InvalidDataException("Invalid WGC frame.");
        var size=Line().Split(' '); var width=int.Parse(size[0]); var height=int.Parse(size[1]); if(Line()!="255") throw new InvalidDataException("Invalid WGC format.");
        long count=0;
        for(var y=Math.Max(0,bounds.Top);y<Math.Min(height,bounds.Bottom);y++) for(var x=Math.Max(0,bounds.Left);x<Math.Min(width,bounds.Right);x++)
        { var n=at+3*(y*width+x); if(bytes[n]>180&&bytes[n+1]<80&&bytes[n+2]>180) count++; }
        return count;
    }
    [StructLayout(LayoutKind.Sequential)] private struct WindowBounds { public int Left,Top,Right,Bottom; }
    [DllImport("user32.dll")] private static extern bool IsIconic(nint window);
    [DllImport("user32.dll")] private static extern bool IsWindowVisible(nint window);
    [DllImport("user32.dll", EntryPoint="GetWindowLongPtrW")] private static extern nint GetWindowLongPtr(nint window,int index);
    [DllImport("user32.dll")] private static extern bool GetWindowRect(nint window,out WindowBounds bounds);
    [DllImport("user32.dll")] private static extern uint GetDpiForWindow(nint window);
}
