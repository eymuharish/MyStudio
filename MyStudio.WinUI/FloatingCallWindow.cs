using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Windowing;
using System.Diagnostics;
using System.Runtime.InteropServices;
using TeleStudio.Contracts;
namespace MyStudio.WinUI;

/// <summary>Compact independent controls. Only the grip moves the window.</summary>
public sealed class FloatingCallWindow : Window
{
    internal const int LogicalWidth = 504, LogicalHeight = 48;
    private readonly StackPanel panel = new() { Padding = new Thickness(6), Background = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 21, 34, 48)), RequestedTheme = ElementTheme.Dark };
    private readonly Button grip = new(), microphone = new(), roomAudio = new(), systemAudio = new(), screenPause = new(), start = new(), pause = new(), stop = new(), end = new(), hide = new();
    private readonly TextBlock callTimer = Timer(Windows.UI.Color.FromArgb(255, 104, 230, 145));
    private readonly TextBlock recordingTimer = Timer(Windows.UI.Color.FromArgb(255, 255, 110, 120));
    private readonly Func<Task> screenPauseAction;
    private IEnumerable<Button> IconButtons => new[] { grip, microphone, roomAudio, systemAudio, screenPause, start, pause, stop, end, hide };
    internal bool IconsOnly => IconButtons.All(button => button.Content is IconElement || button.Content is Grid grid && grid.Children.All(child => child is IconElement or Border || child is Viewbox box && box.Child is IconElement));
    internal bool AccessibleIcons => IconButtons.All(button => !string.IsNullOrWhiteSpace(AutomationProperties.GetName(button)) && ToolTipService.GetToolTip(button) is string { Length: > 0 });
    internal Task InvokeScreenPauseForTestAsync() => screenPause.IsEnabled ? screenPauseAction() : throw new InvalidOperationException("Screen sharing pause icon is disabled.");
    private bool disposing, visible, bounding;
    private readonly CallBarVisibility visibility = new();
    private uint? dragPointerId;
    private NativePoint pointerStart;
    private Windows.Graphics.PointInt32 windowStart;
    private double lastScale;
    private long nextTopmostRefresh;
    private bool? previousMuted, previousMicEnabled;
    private bool previousArabic;
    public bool CaptureExcluded { get; private set; }
    public int CaptureExclusionError { get; private set; }
    public FrameworkElement CaptureTarget => panel;
    internal TextBlock CallTimerForSmoke => callTimer;
    internal TextBlock RecordingTimerForSmoke => recordingTimer;
    internal IReadOnlyList<Button> AudioButtonsForSmoke => [microphone, roomAudio, systemAudio];
    internal IReadOnlyList<Border> ControlGroupsForSmoke => ((StackPanel)((Viewbox)panel.Children.OfType<Viewbox>().Single()).Child).Children.OfType<Border>().ToArray();
    public nint Handle => WinRT.Interop.WindowNative.GetWindowHandle(this);
    internal void SetCaptureExclusionForTest(bool enabled) { CaptureExcluded = SetWindowDisplayAffinity(Handle, enabled ? 0x11u : 0u); CaptureExclusionError = CaptureExcluded ? 0 : Marshal.GetLastWin32Error(); }
    internal void ShowCaptureMarkerForTest(bool show)
    {
        if (show) panel.Children.Insert(0, new Border { Height = 40, Background = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 255, 0, 255)), Tag = "capture-marker" });
        else foreach (var child in panel.Children.OfType<FrameworkElement>().Where(x => Equals(x.Tag, "capture-marker")).ToArray()) panel.Children.Remove(child);
    }
    public FloatingCallWindow(Window main, Func<Task> toggleMicrophone, Func<Task> toggleRoomAudio, Func<Task> toggleSystemAudio, Func<Task> toggleScreenPause,
        Func<Task> startRecording, Func<Task> pauseRecording, Func<Task> stopRecording, Func<Task> hangup)
    {
        Title = "MyStudio call"; Content = panel;
        screenPauseAction = toggleScreenPause;
        var controls = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 4 };
        foreach (var button in IconButtons)
        {
            button.Width = button.MinWidth = button == grip || button == hide ? 24 : 28;
            button.Height = button.MinHeight = 28; button.Padding = new Thickness(0);
        }
        end.Background = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 155, 35, 45));
        controls.Children.Add(grip);
        controls.Children.Add(Group(Windows.UI.Color.FromArgb(255, 72, 129, 99), callTimer, microphone, roomAudio, end));
        controls.Children.Add(Group(Windows.UI.Color.FromArgb(255, 67, 126, 169), screenPause, systemAudio));
        controls.Children.Add(Group(Windows.UI.Color.FromArgb(255, 158, 78, 87), recordingTimer, start, pause, stop));
        controls.Children.Add(hide);
        // Fit all controls together when a small monitor or high DPI reduces the work area.
        panel.Children.Add(new Viewbox { Stretch = Stretch.Uniform, StretchDirection = StretchDirection.DownOnly, Child = controls });
        microphone.Click += async (_, _) => await toggleMicrophone();
        roomAudio.Click += async (_, _) => await toggleRoomAudio(); systemAudio.Click += async (_, _) => await toggleSystemAudio();
        screenPause.Click += async (_, _) => await toggleScreenPause();
        start.Click += async (_, _) => await startRecording(); pause.Click += async (_, _) => await pauseRecording();
        stop.Click += async (_, _) => await stopRecording(); end.Click += async (_, _) => await hangup();
        hide.Click += (_, _) => Hide();
        grip.AddHandler(UIElement.PointerPressedEvent, new Microsoft.UI.Xaml.Input.PointerEventHandler((_, args) =>
        {
            if (!args.GetCurrentPoint(grip).Properties.IsLeftButtonPressed || !GetCursorPos(out pointerStart)) return;
            windowStart = AppWindow.Position;
            if (grip.CapturePointer(args.Pointer)) { dragPointerId = args.Pointer.PointerId; args.Handled = true; }
        }), true);
        grip.AddHandler(UIElement.PointerMovedEvent, new Microsoft.UI.Xaml.Input.PointerEventHandler((_, args) =>
        {
            if (dragPointerId != args.Pointer.PointerId || !GetCursorPos(out var current)) return;
            var dx = current.X - pointerStart.X; var dy = current.Y - pointerStart.Y;
            if (!CallBarBounds.ShouldDrag(dx / Math.Max(1, lastScale), dy / Math.Max(1, lastScale))) return;
            MoveWithinArea(windowStart.X + dx, windowStart.Y + dy); args.Handled = true;
        }), true);
        grip.AddHandler(UIElement.PointerReleasedEvent, new Microsoft.UI.Xaml.Input.PointerEventHandler((_, args) => { if (dragPointerId == args.Pointer.PointerId) { dragPointerId = null; grip.ReleasePointerCapture(args.Pointer); args.Handled = true; } }), true);
        grip.PointerCaptureLost += (_, _) => dragPointerId = null;
        grip.KeyDown += (_, args) =>
        {
            var position = AppWindow.Position; var step = (int)Math.Ceiling(10 * Math.Max(1, lastScale));
            var dx = args.Key == Windows.System.VirtualKey.Left ? -step : args.Key == Windows.System.VirtualKey.Right ? step : 0;
            var dy = args.Key == Windows.System.VirtualKey.Up ? -step : args.Key == Windows.System.VirtualKey.Down ? step : 0;
            if (dx != 0 || dy != 0) { MoveWithinArea(position.X + dx, position.Y + dy); args.Handled = true; }
        };
        var presenter = OverlappedPresenter.Create(); presenter.IsAlwaysOnTop = true; presenter.IsResizable = false;
        presenter.IsMinimizable = false; presenter.IsMaximizable = false; presenter.SetBorderAndTitleBar(false, false); AppWindow.SetPresenter(presenter);
        var area = DisplayArea.GetFromWindowId(main.AppWindow.Id, DisplayAreaFallback.Primary).WorkArea;
        lastScale = Math.Max(1, GetDpiForWindow(WinRT.Interop.WindowNative.GetWindowHandle(main)) / 96d);
        ApplyBounds(area.X + area.Width - (int)(LogicalWidth * lastScale) - 12, area.Y + 24, area);
        AppWindow.Changed += (_, args) => { if (!bounding && (args.DidPositionChange || args.DidSizeChange)) FitToDisplay(); };
        panel.Loaded += (_, _) => { panel.XamlRoot.Changed += (_, _) => FitToDisplay(); FitToDisplay(); };
        CaptureExcluded = SetWindowDisplayAffinity(Handle, 0x11); CaptureExclusionError = CaptureExcluded ? 0 : Marshal.GetLastWin32Error();
        AppWindow.Closing += (_, args) => { if (!disposing) { args.Cancel = true; Hide(); } };
    }
    private void ApplyBounds(int x, int y, Windows.Graphics.RectInt32 area)
    {
        var bounds = CallBarBounds.Clamp(x, y, (int)Math.Ceiling(LogicalWidth * lastScale), (int)Math.Ceiling(LogicalHeight * lastScale), area.X, area.Y, area.Width, area.Height);
        if (AppWindow.Position.X == bounds.X && AppWindow.Position.Y == bounds.Y && AppWindow.Size.Width == bounds.Width && AppWindow.Size.Height == bounds.Height) return;
        bounding = true;
        try { AppWindow.MoveAndResize(new(bounds.X, bounds.Y, bounds.Width, bounds.Height)); }
        finally { bounding = false; }
    }
    private void MoveWithinArea(int x, int y)
    {
        var area = DisplayArea.GetFromPoint(new Windows.Graphics.PointInt32(x + AppWindow.Size.Width / 2, y + AppWindow.Size.Height / 2), DisplayAreaFallback.Nearest).WorkArea;
        ApplyBounds(x, y, area);
    }
    private void FitToDisplay()
    {
        lastScale = Math.Max(1, GetDpiForWindow(Handle) / 96d);
        ApplyBounds(AppWindow.Position.X, AppWindow.Position.Y, DisplayArea.GetFromWindowId(AppWindow.Id, DisplayAreaFallback.Nearest).WorkArea);
    }
    private void Hide() { visibility.Hide(); AppWindow.Hide(); visible = false; }
    public void Reopen() { visibility.Reopen(); FitToDisplay(); AppWindow.Show(false); visible = true; KeepTopmost(true); }
    public void Update(bool active, string state, TimeSpan elapsed, RecordingStateChanged recording, bool canRecord, bool busy, bool arabic,
        bool micMuted, bool canMute, bool roomMuted, bool canMuteRoom, bool systemEnabled, bool canShareSystemAudio,
        bool canPauseOrResume, ScreenSharingPauseView sharing, bool? systemRequested = null)
    {
        panel.FlowDirection = arabic ? FlowDirection.RightToLeft : FlowDirection.LeftToRight;
        var timers = CallBarTimers.Create(elapsed, recording.ElapsedMs);
        if (callTimer.Text != timers.Call) callTimer.Text = timers.Call;
        if (recordingTimer.Text != timers.Recording) recordingTimer.Text = timers.Recording;
        var callDetail = state + " · " + timers.Call;
        var recordDetail = (recording.Paused ? (arabic ? "متوقف مؤقتاً" : "Paused") : recording.State.ToString()) + " · " + timers.Recording;
        DescribeTimer(callTimer, arabic ? "وقت المكالمة الحالية" : "Current call duration", callDetail);
        DescribeTimer(recordingTimer, arabic ? "وقت التسجيل" : "Recording duration", recordDetail);
        if (grip.Content is not FontIcon) grip.Content = new FontIcon { Glyph = "⠿", FontFamily = new FontFamily("Segoe UI"), FontSize = 20 };
        Describe(grip, arabic ? "نقل شريط المكالمة" : "Move call bar", (arabic ? "اسحب أو استخدم الأسهم" : "Drag or use arrow keys") + " · " + callDetail);
        RecordingIcon(hide, Symbol.Cancel, arabic ? "إخفاء الشريط؛ تستمر المكالمة" : "Hide bar; the call continues");
        microphone.IsEnabled = canMute && !busy;
        if (previousMuted != micMuted || previousMicEnabled != microphone.IsEnabled || previousArabic != arabic)
        {
            microphone.Content = CallControlVisuals.Icon(Symbol.Microphone, micMuted);
            Describe(microphone, micMuted ? (arabic ? "تشغيل الميكروفون" : "Unmute microphone") : (arabic ? "كتم الميكروفون" : "Mute microphone"));
            previousMuted = micMuted; previousMicEnabled = microphone.IsEnabled; previousArabic = arabic;
        }
        AudioIcon(roomAudio, Symbol.Volume, roomMuted, canMuteRoom && !busy,
            roomMuted ? (arabic ? "تشغيل صوت القاعة" : "Unmute room audio") : (arabic ? "كتم صوت القاعة" : "Mute room audio"));
        AudioIcon(systemAudio, Symbol.Audio, !systemEnabled, canShareSystemAudio && !busy,
            systemRequested is { } requested ? (requested ? (arabic ? "بانتظار بدء صوت الجهاز" : "Waiting for system audio to start") : (arabic ? "بانتظار إيقاف صوت الجهاز" : "Waiting for system audio to stop")) :
            systemEnabled ? (arabic ? "إيقاف مشاركة صوت الجهاز" : "Stop sharing system audio") : (arabic ? "مشاركة صوت الجهاز" : "Share system audio"));
        ToggleIcon(screenPause, Symbol.Share, sharing.Paused);
        Describe(screenPause, sharing.Paused ? (arabic ? "استئناف مشاركة الشاشة" : "Resume screen sharing") : (arabic ? "إيقاف مشاركة الشاشة مؤقتاً" : "Pause screen sharing"),
            sharing.Resuming ? (arabic ? "بانتظار تأكيد المشاركة" : "Waiting for sharing confirmation") :
            !sharing.CanToggle ? (arabic ? "اختر شاشة أو نافذة وشاركها؛ الكاميرا لا تستأنف هنا" : "Choose and share a display or window; camera sharing cannot resume here.") : "");
        screenPause.IsEnabled = sharing.CanToggle;
        RecordingIcon(start, null, arabic ? "بدء تسجيل المصدر المشترك" : "Record shared source",
            (canRecord ? "" : arabic ? "شارك شاشة أو نافذة قبل التسجيل · " : OpalRecordingPolicy.SharingRequired + " · ") + recordDetail);
        RecordingIcon(pause, recording.Paused ? Symbol.Play : Symbol.Pause,
            recording.Paused ? (arabic ? "استئناف التسجيل" : "Resume recording") : (arabic ? "إيقاف التسجيل مؤقتاً" : "Pause recording"),
            recording.Paused && !canPauseOrResume ? (arabic ? "شارك شاشة أو نافذة ثم استأنف التسجيل" : "Share a display or window, then resume recording.") : "");
        RecordingIcon(stop, Symbol.Stop, arabic ? "إيقاف التسجيل" : "Stop recording");
        RecordingIcon(end, Symbol.HangUp, arabic ? "إنهاء المكالمة" : "Hang up", callDetail);
        start.IsEnabled = canRecord && !busy; pause.IsEnabled = canPauseOrResume && !busy;
        stop.IsEnabled = (recording.State is RecordingState.Starting or RecordingState.Recording) && !busy; end.IsEnabled = active && !busy;
        var shouldShow = visibility.ShouldShow(active); if (!shouldShow && visible) { AppWindow.Hide(); visible = false; }
        else if (shouldShow && !visible) Reopen();
        if (visible) KeepTopmost();
    }
    private static TextBlock Timer(Windows.UI.Color color) => new()
    {
        Text = "00:00:00", Width = 72, FontSize = 13, FontFamily = new FontFamily("Consolas"),
        Foreground = new SolidColorBrush(color), VerticalAlignment = VerticalAlignment.Center,
        TextAlignment = TextAlignment.Center, FlowDirection = FlowDirection.LeftToRight
    };
    private static Border Group(Windows.UI.Color color, params UIElement[] controls)
    {
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 3 };
        foreach (var control in controls) row.Children.Add(control);
        return new Border { BorderBrush = new SolidColorBrush(color), BorderThickness = new Thickness(1),
            CornerRadius = new CornerRadius(5), Padding = new Thickness(4, 3, 4, 3), Child = row };
    }
    private static void DescribeTimer(TextBlock timer, string name, string detail)
    {
        if (AutomationProperties.GetName(timer) == name && AutomationProperties.GetHelpText(timer) == detail) return;
        AutomationProperties.SetName(timer, name); AutomationProperties.SetHelpText(timer, detail);
        ToolTipService.SetToolTip(timer, detail);
    }
    private static void AudioIcon(Button button, Symbol symbol, bool off, bool available, string action)
    {
        if (!Equals(button.Tag, (symbol, off))) { button.Content = CallControlVisuals.Icon(symbol, off); button.Tag = (symbol, off); }
        button.IsEnabled = available; Describe(button, action);
    }
    private void KeepTopmost(bool force = false)
    {
        var now = Stopwatch.GetTimestamp();
        if (!force && now < nextTopmostRefresh) return;
        nextTopmostRefresh = now + Stopwatch.Frequency;
        // Restore the topmost Z-order without moving, resizing, or stealing focus.
        SetWindowPos(Handle, new nint(-1), 0, 0, 0, 0, 0x0013 | 0x0200);
    }
    private static void ToggleIcon(Button button, Symbol symbol, bool resume)
    {
        if (Equals(button.Tag, (symbol, resume))) return;
        var grid = new Grid { Width = 22, Height = 22 };
        grid.Children.Add(new SymbolIcon(symbol) { Margin = new Thickness(0, 0, 5, 5) });
        grid.Children.Add(new Viewbox { Width = 10, Height = 10, HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Bottom,
            Child = new SymbolIcon(resume ? Symbol.Play : Symbol.Pause) { Width = 20, Height = 20 } });
        button.Content = grid; button.Tag = (symbol, resume);
    }
    private static void Describe(Button button, string name, string help = "")
    {
        var description = help.Length > 0 ? help : name;
        if (AutomationProperties.GetName(button) == name && AutomationProperties.GetHelpText(button) == description) return;
        AutomationProperties.SetName(button, name); AutomationProperties.SetHelpText(button, description);
        ToolTipService.SetToolTip(button, help.Length > 0 ? name + " · " + help : name);
    }
    private static void RecordingIcon(Button button, Symbol? symbol, string name, string help = "")
    {
        if (symbol is { } value)
        {
            if (button.Content is not SymbolIcon icon || icon.Symbol != value) button.Content = new SymbolIcon(value);
        }
        else if (button.Content is not FontIcon) button.Content = new FontIcon { Glyph = "●", FontFamily = new FontFamily("Segoe UI"), FontSize = 20 };
        Describe(button, name, help);
    }
    public void Dispose() { disposing = true; Close(); }
    [StructLayout(LayoutKind.Sequential)] private struct NativePoint { public int X, Y; }
    [DllImport("user32.dll")] private static extern bool GetCursorPos(out NativePoint point);
    [DllImport("user32.dll")] private static extern uint GetDpiForWindow(nint window);
    [DllImport("user32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetWindowPos(nint window, nint insertAfter, int x, int y, int width, int height, uint flags);
    [DllImport("user32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)] private static extern bool SetWindowDisplayAffinity(nint window, uint affinity);
}

