using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using TeleStudio.Contracts;
using TeleStudio.Desktop;
using TeleStudio.Settings;
namespace MyStudio.WinUI;
public sealed partial class MainWindow
{
    private readonly ComboBox recentDestinations = new() { FlowDirection = FlowDirection.LeftToRight };
    private Button? redial;
    private readonly StackPanel recordingsList = new() { Spacing = 12 };
    private readonly RecordingFileActions recordingFiles = new();
    private void RefreshRecentDestinations() => recentDestinations.ItemsSource = session.Settings.RecentDestinations.Where(RecentDestinationPolicy.IsSafe).ToArray();
    private void BuildRecordingsPage()
    {
        var body = Stack(); body.Children.Add(Text("التسجيلات", "Recordings", 30));
        body.Children.Add(Text("التسجيلات المكتملة محفوظة كملفات MP4", "Completed recordings are saved as MP4 files"));
        var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        actions.Children.Add(Action("تحديث", "Refresh", () => { RefreshRecordings(); return Task.CompletedTask; }));
        actions.Children.Add(Action("فتح المجلد", "Open folder", () => { if (!recordingFiles.TryOpenFolder(out var error)) throw new InvalidOperationException(error); return Task.CompletedTask; }));
        actions.Children.Add(Action("اختيار المجلد", "Choose folder", ChooseRecordingFolderAsync));
        body.Children.Add(actions); body.Children.Add(recordingsList); Page("recordings", body);
    }
    private void RefreshRecordings()
    {
        recordingsList.Children.Clear();
        try
        {
            var files = recordingFiles.ListCompletedFiles();
            if (files.Count == 0) recordingsList.Children.Add(new TextBlock { Text = T("لا توجد تسجيلات مكتملة في المجلد المختار.", "No completed recordings in the selected folder.") });
            foreach (var file in files)
            {
                var row = new StackPanel { Spacing = 6 }; row.Children.Add(new TextBlock { Text = file.FileName, FontSize = 18, TextWrapping = TextWrapping.Wrap, FlowDirection = FlowDirection.LeftToRight });
                row.Children.Add(new TextBlock { Text = $"{file.Modified:yyyy-MM-dd HH:mm} · {file.Bytes / 1048576d:F2} MB", FlowDirection = FlowDirection.LeftToRight });
                var open = new Button { Content = T("فتح التسجيل", "Open recording"), MinHeight = 40 };
                open.Click += async (_, _) => await RunAsync(() => { if (!recordingFiles.TryOpenFile(file.FileName, out var error)) throw new InvalidOperationException(error); return Task.CompletedTask; }); row.Children.Add(open);
                recordingsList.Children.Add(row);
            }
            foreach (var item in translations) item.Set(T(item.Ar, item.En));
        }
        catch (Exception error) { ReportError("recording-library", error.Message); }
    }
    private sealed record SharingOption(CaptureCatalogItem Item, string Name);
    private sealed record SharingModeOption(SharingTransmissionMode Mode, string Name);
    private SharingTransmissionMode sharingTransmissionMode = SharingTransmissionMode.Auto;
    private async Task ShowSharingSourceAsync()
    {
        using var dialogTranslations = translations.BeginScope();
        if (session.Calls?.IsConnected != true || capabilities is not { SourceControl: true, VideoControl: true }) throw new InvalidOperationException("Sharing is unavailable for this call.");
        var displays = await session.Catalog!.RefreshAsync(lifetime.Token);
        var cameras = await session.Cameras!.RefreshAsync(lifetime.Token);
        var options = cameras.Items.Concat(displays.Items).Where(x => x.Kind is CaptureItemKind.Camera or CaptureItemKind.Monitor or CaptureItemKind.Window)
            .Select(x => new SharingOption(x, (x.Kind switch { CaptureItemKind.Camera => T("كاميرا", "Camera"), CaptureItemKind.Monitor => T("شاشة", "Display"), _ => T("نافذة", "Window") }) + " · " + x.Name)).ToArray();
        var modes = new[] {
            new SharingModeOption(SharingTransmissionMode.Auto, T("تلقائي (موصى به)", "Automatic (recommended)")),
            new SharingModeOption(SharingTransmissionMode.MainOnly, T("الفيديو الرئيسي فقط", "Main video only")),
            new SharingModeOption(SharingTransmissionMode.MainAndPresentation, T("الرئيسي + قناة العرض", "Main + presentation"))
        };
        var mode = new ComboBox { Name = "SharingTransmissionMode", DisplayMemberPath = "Name", ItemsSource = modes, SelectedItem = modes.Single(x => x.Mode == sharingTransmissionMode) };
        var source = new ComboBox { Name = "SharingSource", DisplayMemberPath = "Name", ItemsSource = options, SelectedIndex = -1 };
        var panel = Stack(); Field(panel, "نمط إرسال مشاركة الشاشة والنافذة", "Screen/window transmission mode", mode);
        panel.Children.Add(Text("تُستخدم قناة العرض إذا قبلها الجهاز، وإلا تُرسل المشاركة على الفيديو الرئيسي. عدد شاشات القاعة تحدده إعدادات جهازها.", "Presentation is used when accepted; otherwise sharing uses main video. Room display coverage depends on the room device settings."));
        Field(panel, "اختر كاميرا أو شاشة أو نافذة لمشاركتها", "Choose a camera, display or window to share", source);
        panel.Children.Add(Text("الكاميرا متوقفة حتى تختارها هنا. تبدأ المشاركة بعد اختيار المصدر وتطبيقه.", "The camera stays off until you choose it here. Sharing starts after you select a source and apply."));
        if (!await DialogAsync("مشاركة من", "Share from", panel, dialog => {
            dialog.IsPrimaryButtonEnabled = false;
            source.SelectionChanged += (_, _) => { dialog.IsPrimaryButtonEnabled = source.SelectedItem is SharingOption; mode.IsEnabled = source.SelectedItem is not SharingOption { Item.Kind: CaptureItemKind.Camera }; };
        })) return;
        if (source.SelectedItem is not SharingOption selected) throw new InvalidOperationException("Select a source before sharing.");
        if (selected.Item.Kind != CaptureItemKind.Camera) sharingTransmissionMode = ((SharingModeOption)mode.SelectedItem).Mode;
        await SelectSharingSourceAsync(selected.Item);
    }
    private async Task SelectSharingSourceAsync(CaptureCatalogItem selected, bool preservePausedSource = false)
    {
        var commands = OpalSharingPolicy.Select(selected, videoState, sharingTransmissionMode);
        var wasCaptureExcluded = mainCaptureExcluded;
        if (!ApplySharingCaptureExclusion(selected.Kind))
            throw new InvalidOperationException("Cannot prepare the selected screen for sharing. Please retry the source selection.");
        if (!preservePausedSource) screenSharingPause.Clear();
        sharedSource = null; videoState = null; Render();
        try { foreach (var command in commands) await SendAsync(command); }
        catch { ApplySharingCaptureExclusion(wasCaptureExcluded ? CaptureItemKind.Monitor : null); throw; }
        sharedSource = selected;
        if (selected.Kind == CaptureItemKind.Camera) camera.SelectedItem = selected;
        else desktop.SelectedItem = selected;
        Render();
    }
    private async Task StopSharingAsync()
    {
        var commands = OpalSharingPolicy.Stop(videoState); screenSharingPause.Clear(); sharedSource = null; Render();
        foreach (var command in commands) await SendAsync(command);
        ApplySharingCaptureExclusion(null);
    }
    private async Task ToggleScreenSharingPauseAsync()
    {
        // RunAsync marks this action busy before invoking it; validate media readiness without that UI guard.
        if (!GetScreenSharingPauseControls(false).CanToggle) throw new InvalidOperationException("Screen sharing is not ready to pause or resume.");
        if (screenSharingPause.RememberedSource is not null)
        {
            var selected = screenSharingPause.RequestResume(videoState);
            try { await SelectSharingSourceAsync(selected, preservePausedSource: true); }
            catch { screenSharingPause.CancelResumeRequest(); Render(); throw; }
        }
        else
        {
            var command = screenSharingPause.Pause(sharedSource, videoState);
            sharedSource = null; Render();
            try { await SendAsync(command); }
            catch { screenSharingPause.Clear(); Render(); throw; }
        }
    }
}
