using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Windowing;
using System.IO;
using System.Text.Json;
using TeleStudio.Contracts;
using TeleStudio.Desktop;
using TeleStudio.Presentation;
using TeleStudio.Settings;

namespace MyStudio.WinUI;

public sealed partial class MainWindow
{
    // A local UI fixture only: no dialing, recording, capture source or media commands.
    private async Task<bool> RunCallBarSmokeIfRequestedAsync()
    {
        var args = Environment.GetCommandLineArgs(); var at = Array.IndexOf(args, "--call-bar-smoke");
        if (at < 0) return false;
        if (at + 1 >= args.Length) { await ShutdownAsync(); return true; }
        var folder = Path.GetFullPath(args[at + 1]); Directory.CreateDirectory(folder);
        using var limit = new CancellationTokenSource(TimeSpan.FromSeconds(25)); var token = limit.Token;
        FloatingCallWindow? fixture = null;
        var snapshots = new List<object>();
        try
        {
            VerifyTransientTranslationScopeForSmoke();
            if (!session.Ready) throw new InvalidOperationException("Call-bar smoke requires a ready OPAL worker: " + session.Error);
            var parsed = AppDataPaths.ParseArguments(args.Skip(1).ToArray());
            if (parsed.Root is null || Path.GetFullPath(parsed.Root).Equals(Path.Combine(AppContext.BaseDirectory, "OPALData"), StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException("Call-bar QA requires an explicit isolated --data-root folder.");
            if (session.Calls?.CanHangUp == true) throw new InvalidOperationException("Call-bar QA must start without an active call.");
            var audioSink = new CallBarAudioFixtureSink();
            using var audioScheduler = new DebouncedAudioCommitScheduler();
            var audio = new InCallAudioViewModel(audioSink, audioScheduler);
            audio.UpdateAvailability(true, true, true, true, true);
            Func<Task> audioAction = () => audio.ToggleSystemAudioMuteAsync(token);
            fixture = new(this, NoAction, NoAction, audioAction, NoAction, NoAction, NoAction, NoAction, NoAction);
            void Update(bool ar, long callSeconds = 754, long recordedMs = 83000, bool paused = false,
                bool allowed = true, bool roomMuted = false, bool systemEnabled = true, bool? systemRequested = null) =>
                fixture.Update(true, ar ? "المكالمة متصلة" : "Call connected", TimeSpan.FromSeconds(callSeconds),
                    new RecordingStateChanged(RecordingState.Recording, true, true, recordedMs, "local-ui-fixture.mp4", Paused: paused),
                    false, false, ar, false, allowed, roomMuted, allowed, systemEnabled, allowed,
                    allowed, new ScreenSharingPauseView(allowed, false, false), systemRequested);

            foreach (var ar in new[] { false, true })
            {
                Update(ar); await Task.Delay(250, token);
                VerifyCallHoldRemoved(fixture);
                CheckTimer(fixture.CallTimerForSmoke, "00:12:34", green: true);
                CheckTimer(fixture.RecordingTimerForSmoke, "00:01:23", green: false);
                if (!fixture.IconsOnly || !fixture.AccessibleIcons || fixture.AudioButtonsForSmoke.Any(x => !x.IsEnabled))
                    throw new InvalidOperationException("Fixture audio controls are unavailable or inaccessible.");
                if (fixture.ControlGroupsForSmoke.Count != 3 || fixture.ControlGroupsForSmoke.Any(x => x.BorderThickness.Left <= 0))
                    throw new InvalidOperationException("Call, sharing and recording groups lack their visible borders.");
                var observedBounds = CheckCallBarFits(fixture);
                var language = ar ? "ar" : "en";
                await SaveCaptureAsync(Path.Combine(folder, "call-bar-" + language + ".png"), fixture.CaptureTarget).WaitAsync(token);
                snapshots.Add(new { language, call = fixture.CallTimerForSmoke.Text, recording = fixture.RecordingTimerForSmoke.Text, bounds = observedBounds });
            }
            Update(false, callSeconds: 755, paused: true); await Task.Delay(100, token);
            if (fixture.CallTimerForSmoke.Text != "00:12:35" || fixture.RecordingTimerForSmoke.Text != "00:01:23")
                throw new InvalidOperationException("Pausing backend recording affected the call timer or advanced the recording timer.");
            Update(false, callSeconds: 756, paused: true); await Task.Delay(100, token);
            if (fixture.CallTimerForSmoke.Text != "00:12:36" || fixture.RecordingTimerForSmoke.Text != "00:01:23")
                throw new InvalidOperationException("Paused recording did not remain frozen while call time increased.");
            Update(false, allowed: false);
            if (fixture.AudioButtonsForSmoke.Any(x => x.IsEnabled)) throw new InvalidOperationException("Audio controls bypassed disabled capability permissions.");
            Update(false, allowed: false, systemEnabled: false, systemRequested: true);
            if (fixture.AudioButtonsForSmoke[2].IsEnabled || Microsoft.UI.Xaml.Automation.AutomationProperties.GetName(fixture.AudioButtonsForSmoke[2]) != "Waiting for system audio to start")
                throw new InvalidOperationException("Pending system audio permits duplicate commands or claims unavailable/active capture.");
            await audioAction();
            var repeatedBlocked = false;
            try { await audioAction(); }
            catch (InvalidOperationException) { repeatedBlocked = true; }
            if (!repeatedBlocked || audioSink.Commands.Count != 1 || !audio.IsSystemAudioPending)
                throw new InvalidOperationException("Repeated toggle restarted pending capture.");
            if (audioSink.Commands.Single() is not SetSystemAudioEnabled { Enabled: true }) throw new InvalidOperationException("The fixture callback did not request system audio once.");
            audio.ApplySystemAudioConfirmation(new SystemAudioChanged(false));
            if (!audio.IsSystemAudioPending) throw new InvalidOperationException("A stale initial snapshot cleared the requested activation.");
            audio.ApplySystemAudioConfirmation(new SystemAudioChanged(true));
            Update(false, allowed: audio.CanToggleSystemAudioMute, systemEnabled: audio.ConfirmedSystemAudioEnabled);
            if (!fixture.AudioButtonsForSmoke[2].IsEnabled || Microsoft.UI.Xaml.Automation.AutomationProperties.GetName(fixture.AudioButtonsForSmoke[2]) != "Stop sharing system audio")
                throw new InvalidOperationException("Confirmed activation did not restore the stop action.");
            audio.UpdateAvailability(false, false);
            Update(false, roomMuted: true, systemEnabled: false); await Task.Delay(100, token);
            if (Microsoft.UI.Xaml.Automation.AutomationProperties.GetName(fixture.AudioButtonsForSmoke[1]) != "Unmute room audio" ||
                Microsoft.UI.Xaml.Automation.AutomationProperties.GetName(fixture.AudioButtonsForSmoke[2]) != "Share system audio")
                throw new InvalidOperationException("Audio actions do not reflect confirmed mute/share state.");
            var presenter = (OverlappedPresenter)AppWindow.Presenter;
            presenter.Minimize(); await Task.Delay(1300, token);
            var main = WinRT.Interop.WindowNative.GetWindowHandle(this);
            if (!IsIconic(main) || !IsWindowVisible(fixture.Handle) || (GetWindowLongPtr(fixture.Handle, -20).ToInt64() & 8) == 0)
                throw new InvalidOperationException("Call bar lost visibility or topmost style when the main window minimized.");
            await SaveCaptureAsync(Path.Combine(folder, "call-bar-main-minimized.png"), fixture.CaptureTarget).WaitAsync(token);
            presenter.Restore();
            File.WriteAllText(Path.Combine(folder, "call-bar-smoke.json"), JsonSerializer.Serialize(new {
                result = "PASS", fixtureOnly = true, realDeviceCalls = 0, engineReady = session.Ready, transientTranslationBindingsReleased = true,
                dataRoot = AppDataPaths.Root, recordingPauseDoesNotPauseCall = true, disabledPermissionsHonored = true,
                confirmedAudioActions = true, asyncAudioAcknowledgement = true, duplicateAudioRequestsPrevented = true, survivesMainMinimize = true, topmostStyle = true,
                callHoldControlsAbsent = true, logicalWidth = FloatingCallWindow.LogicalWidth, logicalHeight = FloatingCallWindow.LogicalHeight, snapshots
            }, new JsonSerializerOptions { WriteIndented = true }));
        }
        catch (Exception error) { File.WriteAllText(Path.Combine(folder, "call-bar-smoke-error.txt"), error.ToString()); }
        finally { fixture?.Dispose(); await ShutdownAsync(); }
        return true;
    }
    private static Task NoAction() => Task.CompletedTask;
    private void VerifyCallHoldRemoved(FloatingCallWindow fixture)
    {
        var counts = fixture.ControlGroupsForSmoke.Select(group => ((StackPanel)group.Child).Children.OfType<Button>().Count());
        if (!counts.SequenceEqual(new[] { 3, 2, 3 }))
            throw new InvalidOperationException("Floating call controls still contain call hold or lost another control.");
        var forbidden = new HashSet<string>(StringComparer.Ordinal) { "Hold", "Resume", "تعليق", "استئناف", "Hold call", "Resume call", "تعليق المكالمة", "استئناف المكالمة" };
        void Inspect(DependencyObject item)
        {
            if (item is Button button && (button.Content is string label && forbidden.Contains(label) ||
                forbidden.Contains(Microsoft.UI.Xaml.Automation.AutomationProperties.GetName(button))))
                throw new InvalidOperationException("A call-hold action remains in the user interface.");
            for (var index = 0; index < VisualTreeHelper.GetChildrenCount(item); index++) Inspect(VisualTreeHelper.GetChild(item, index));
        }
        Inspect(Content); Inspect(fixture.CaptureTarget);
    }
    private sealed class CallBarAudioFixtureSink : IEngineCommandSink
    {
        public List<EngineCommand> Commands { get; } = [];
        public Task SendAsync(EngineCommand command, CancellationToken token) { Commands.Add(command); return Task.CompletedTask; }
    }
    private static void CheckTimer(TextBlock timer, string expected, bool green)
    {
        if (timer.Text != expected || !timer.IsLoaded || timer.ActualWidth <= 0 || timer.ActualHeight <= 0 || timer.Foreground is not SolidColorBrush brush ||
            (green ? brush.Color.G <= brush.Color.R : brush.Color.R <= brush.Color.G))
            throw new InvalidOperationException("A visible call/recording timer has incorrect text or color.");
    }
    private static object CheckCallBarFits(FloatingCallWindow fixture)
    {
        var panel = fixture.CaptureTarget;
        var items = fixture.ControlGroupsForSmoke.Cast<FrameworkElement>().Concat(fixture.AudioButtonsForSmoke)
            .Concat(new[] { fixture.CallTimerForSmoke, fixture.RecordingTimerForSmoke });
        foreach (var item in items)
        {
            var transform = item.TransformToVisual(panel);
            var first = transform.TransformPoint(new Windows.Foundation.Point());
            var last = transform.TransformPoint(new Windows.Foundation.Point(item.ActualWidth, item.ActualHeight));
            if (item.ActualWidth <= 0 || item.ActualHeight <= 0 || Math.Min(first.X, last.X) < -1 || Math.Max(first.X, last.X) > panel.ActualWidth + 1 ||
                Math.Min(first.Y, last.Y) < -1 || Math.Max(first.Y, last.Y) > panel.ActualHeight + 1)
                throw new InvalidOperationException("A call-bar group, timer or audio control is clipped.");
        }
        return new { width = panel.ActualWidth, height = panel.ActualHeight };
    }
}
