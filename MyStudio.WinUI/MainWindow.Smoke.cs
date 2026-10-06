using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using System.IO;
using System.Runtime.InteropServices.WindowsRuntime;
using System.Text.Json;
using Windows.Graphics.Imaging;
using Windows.Storage;
using TeleStudio.Contracts;
using TeleStudio.Settings;

namespace MyStudio.WinUI;
public sealed partial class MainWindow
{
    private async Task RunUiSmokeIfRequestedAsync()
    {
        if (await RunCallBarSmokeIfRequestedAsync()) return;
        var arguments = Environment.GetCommandLineArgs(); var index = Array.IndexOf(arguments, "--ui-smoke");
        if (index < 0 || index + 1 >= arguments.Length) return;
        var directory = Path.GetFullPath(arguments[index + 1]); Directory.CreateDirectory(directory);
        var snapshots = new List<object>();
        try
        {
            VerifyTransientTranslationScopeForSmoke();
            if (!session.Ready) throw new InvalidOperationException("UI smoke requires a ready OPAL worker: " + session.Error);
            VerifySharingCaptureExclusion();
            var studioPolicyChecks = VerifyStudioSupportUi();
            var studioSelectionChecks = arguments.Contains("--studio-selection-smoke")
                ? await VerifyKnownStudioSelectionAsync(directory) : [];
            var persistedStartupMode = await new OpalConnectionModeStore(Path.Combine(TeleStudio.Settings.AppDataPaths.Root, "connection-environment.json")).LoadAsync(default);
            if (session.Ready && session.ConnectionMode != persistedStartupMode)
                throw new InvalidOperationException("The startup connection mode does not match the persisted global setting.");
            if (session.Ready)
            {
                dialogCaptureFolder = directory;
                var permanentBindingCount = translations.Count;
                try { await ShowDevicesAsync(); await ShowConnectionAsync(); }
                finally { dialogCaptureFolder = null; }
                if (translations.Count != permanentBindingCount) throw new InvalidOperationException("Closed dialogs retained translation bindings.");
            }
            foreach (var sample in new[] { ("en", "Light", 1280, 820), ("ar", "Dark", 1024, 768), ("en", "Dark", 800, 680) })
            {
                arabic = sample.Item1 == "ar"; theme.SelectedItem = sample.Item2; ApplyLanguage(); ApplyTheme(); AppWindow.Resize(new Windows.Graphics.SizeInt32(sample.Item3, sample.Item4));
                foreach (var page in pages.Keys)
                {
                    ShowPage(page); await Task.Delay(180);
                    object? centeredContent = null;
                    if (pages[page] is ScrollViewer scroll && scroll.Content is FrameworkElement body)
                    {
                        var origin = body.TransformToVisual(scroll).TransformPoint(new Windows.Foundation.Point());
                        var far = body.TransformToVisual(scroll).TransformPoint(new Windows.Foundation.Point(body.ActualWidth, 0));
                        var left = Math.Min(origin.X, far.X); var right = Math.Max(origin.X, far.X);
                        var centerError = Math.Abs((left + right) / 2 - scroll.ActualWidth / 2);
                        if (left < -1 || right > scroll.ActualWidth + 1 || centerError > 12)
                            throw new InvalidOperationException($"Page {page} is clipped or off center: left={left}, right={right}, viewport={scroll.ActualWidth}.");
                        centeredContent = new { left, right, width = body.ActualWidth, viewport = scroll.ActualWidth, centerError };
                    }
                    var visibleConnectionMode = page == "settings" ? VerifyVisibleConnectionMode(persistedStartupMode) : null;
                    var compactStudios = page == "studios" ? VerifyCompactStudioCards() : null;
                    var bitmap = new RenderTargetBitmap(); await bitmap.RenderAsync(navigation);
                    var pixels = await bitmap.GetPixelsAsync(); var file = await StorageFile.GetFileFromPathAsync(await CreateEmptyAsync(Path.Combine(directory, $"{sample.Item1}-{sample.Item2}-{sample.Item3}-{page}.png")));
                    using var stream = await file.OpenAsync(FileAccessMode.ReadWrite); var encoder = await BitmapEncoder.CreateAsync(BitmapEncoder.PngEncoderId, stream);
                    encoder.SetPixelData(BitmapPixelFormat.Bgra8, BitmapAlphaMode.Premultiplied, (uint)bitmap.PixelWidth, (uint)bitmap.PixelHeight, 96, 96, pixels.ToArray()); await encoder.FlushAsync();
                    snapshots.Add(new { page, language = sample.Item1, theme = sample.Item2, width = bitmap.PixelWidth, height = bitmap.PixelHeight, flow = navigation.FlowDirection.ToString(), visibleConnectionMode, centeredContent, compactStudios });
                }
            }
            File.WriteAllText(Path.Combine(directory, "ui-smoke.json"), JsonSerializer.Serialize(new { frontend = "Microsoft.UI.Xaml / WinUI 3", assembly = GetType().Assembly.GetName().Name, engineReady = session.Ready, captureAffinityVerified = true, studioPolicyChecks, studioSelectionChecks, persistedStartupConnectionMode = persistedStartupMode.ToString(), startupConnectionModeMatched = session.ConnectionMode == persistedStartupMode, snapshots }, new JsonSerializerOptions { WriteIndented = true }));
        }
        catch (Exception error) { File.WriteAllText(Path.Combine(directory, "ui-smoke-error.txt"), error.ToString()); }
        finally { await ShutdownAsync(); }
    }
    private string VerifyVisibleConnectionMode(NetworkConnectionMode persistedMode)
    {
        if (persistedMode != NetworkConnectionMode.ExternalVpn || session.ConnectionMode != NetworkConnectionMode.ExternalVpn)
            throw new InvalidOperationException("The automatic VPN connection policy is not applied.");
        void VerifyNoNetworkSelector(DependencyObject parent)
        {
            if (parent is ComboBox selector && selector.Items.Cast<object>().Any(item => item is ComboBoxItem { Tag: NetworkConnectionMode }))
                throw new InvalidOperationException("The retired network selector is still present.");
            if (parent is TextBlock text && (text.Text.Contains("In Building LAN", StringComparison.Ordinal) || text.Text.Contains("داخل المبنى", StringComparison.Ordinal)))
                throw new InvalidOperationException("The retired building option is still visible.");
            for (var index = 0; index < VisualTreeHelper.GetChildrenCount(parent); index++) VerifyNoNetworkSelector(VisualTreeHelper.GetChild(parent, index));
        }
        VerifyNoNetworkSelector(pages["settings"]);
        return "Automatic VPN";
    }
    private async Task<List<object>> VerifyKnownStudioSelectionAsync(string directory)
    {
        session.RequireIdle();
        var result = new List<object>(); var originalMode = session.ConnectionMode;
        foreach (var id in new[] { "018", "047" })
        {
            var studio = session.SupportedStudios.Studios.Single(x => x.Id == id);
            var settingsPath = Path.Combine(TeleStudio.Settings.AppDataPaths.Root, "profiles", studio.Family.ToString(), "settings.json");
            var expectedSettings = File.Exists(settingsPath) ? await new SettingsStore(settingsPath).LoadAsync(default) : OpalEditionDefaults.Settings(studio.Family);
            var expectedInitializations = session.Family == studio.Family ? 0 : 1;
            var initializationCount = session.InitializationCount;
            var entry = session.Rooms.First(x => x.Destination == studio.Address);
            // Invoke the same selection operation as the studio card, with automatic text handling enabled.
            await SelectStudioAsync(entry); Render(); await Task.Delay(180);
            var actualInitializations = session.InitializationCount - initializationCount;
            if (!session.Ready || actualInitializations != expectedInitializations || session.Family != studio.Family ||
                session.Calls!.SelectedEndpoint != studio.EndpointProfile || endpoint.SelectedItem is not EndpointProfile profile || profile != studio.EndpointProfile ||
                session.Calls.SelectedProtocol != studio.PreferredProtocol || protocol.SelectedItem is not CallProtocol selected || selected != studio.PreferredProtocol ||
                session.Tuning.AacEnabled || session.ConnectionMode != originalMode || session.Calls.CanHangUp ||
                resolution.SelectedItem is not VideoResolution video || video != expectedSettings.VideoResolution ||
                bitrate.Value != (expectedSettings.VideoBitrateKbps ?? 2000) || status.IsOpen)
                throw new InvalidOperationException("Known studio selection failed readiness/model/protocol/default/persistence checks: " + id);
            await SaveCaptureAsync(Path.Combine(directory, "selection-" + id + ".png"));
            result.Add(new { studio = id, family = session.Family.ToString(), endpoint = profile.ToString(), protocol = selected.ToString(),
                aacEnabled = session.Tuning.AacEnabled, video = video.ToString(), bitrateKbps = bitrate.Value,
                mode = session.ConnectionMode.ToString(), initializationCount = actualInitializations, callStarted = false });
        }
        return result;
    }
    private void VerifyTransientTranslationScopeForSmoke()
    {
        var original = arabic; var permanent = translations.Count;
        try
        {
            using var scope = translations.BeginScope();
            var label = Text("عنوان مؤقت", "Transient label");
            arabic = true; foreach (var item in translations) item.Set(T(item.Ar, item.En));
            if (label.Text != "عنوان مؤقت") throw new InvalidOperationException("Transient Arabic label translation failed.");
            arabic = false; foreach (var item in translations) item.Set(T(item.Ar, item.En));
            if (label.Text != "Transient label") throw new InvalidOperationException("Transient English label translation failed.");
        }
        finally { arabic = original; foreach (var item in translations) item.Set(T(item.Ar, item.En)); }
        if (translations.Count != permanent) throw new InvalidOperationException("Transient label translation ownership was not released.");
    }
    private object VerifyCompactStudioCards()
    {
        if (studioCards.Children.Count != RoomDirectoryStore.Search(session.Rooms, studioSearch.Text).Count)
            throw new InvalidOperationException("Studio cards omit saved entries.");
        if (studioCards.ActualWidth >= 440 && studioCards.ColumnDefinitions.Count is not (4 or 5))
            throw new InvalidOperationException("Studio cards do not fit four or five columns.");
        foreach (var child in studioCards.Children)
        {
            if (child is not Border { Child: StackPanel card } || card.Children.Count != 2 ||
                card.Children[0] is not TextBlock { TextAlignment: TextAlignment.Center } ||
                card.Children[1] is not Grid { HorizontalAlignment: HorizontalAlignment.Center } icons || icons.Children.Count != 6)
                throw new InvalidOperationException("Studio name/icons are not centered compact card content.");
            foreach (var icon in icons.Children)
                if (icon is not Button { Content: SymbolIcon } button || string.IsNullOrWhiteSpace(ToolTipService.GetToolTip(button) as string) ||
                    string.IsNullOrWhiteSpace(Microsoft.UI.Xaml.Automation.AutomationProperties.GetName(button)))
                    throw new InvalidOperationException("A studio action lacks its icon, tooltip or accessible name.");
        }
        var scroll = (ScrollViewer)pages["studios"];
        if (studioCards.Children.Count <= 15 && scroll.ScrollableHeight > 1)
            throw new InvalidOperationException("Saved studios do not fit on one page.");
        return new { cards = studioCards.Children.Count, columns = studioCards.ColumnDefinitions.Count,
            rows = studioCards.RowDefinitions.Count, namesOnly = true, centeredIcons = true, directCall = true, scrollHeight = scroll.ScrollableHeight };
    }
    private int VerifyStudioSupportUi()
    {
        var original = destination.Text; var originalLanguage = arabic; var checks = 0;
        var prior = rendering; rendering = true;
        try
        {
            foreach (var studio in session.SupportedStudios.Studios)
            foreach (var languageArabic in new[] { false, true })
            {
                arabic = languageArabic; destination.Text = studio.Address; Render();
                var allowed = session.SupportedStudios.AllowedProtocols(studio.Address);
                if (!protocol.Items.Cast<CallProtocol>().SequenceEqual(allowed) ||
                    studioSupportNotes.Text != session.SupportedStudios.Notes(studio.Address, arabic) ||
                    string.IsNullOrWhiteSpace(studioSupportNotes.Text) || family.IsEnabled || endpoint.IsEnabled ||
                    allowed.Length == 0 && dial?.IsEnabled == true)
                    throw new InvalidOperationException("Studio support controls do not match verified catalog policy: " + studio.Id);
                foreach (var connectionProtocol in Enum.GetValues<CallProtocol>())
                {
                    var aac = CreateAacToggle(connectionProtocol, enabled: true, available: true);
                    var supported = session.SupportedStudios.Allows(studio.Address, connectionProtocol, true);
                    if (aac.IsEnabled != supported || aac.IsOn != supported)
                        throw new InvalidOperationException("AAC toggle offers an unverified studio codec: " + studio.Id);
                }
                checks++;
            }
            destination.Text = "192.0.2.22"; Render();
            if (!protocol.Items.Cast<CallProtocol>().SequenceEqual(Enum.GetValues<CallProtocol>()))
                throw new InvalidOperationException("Unknown addresses must retain the manual protocol choices.");
            return checks + 1;
        }
        finally { arabic = originalLanguage; destination.Text = original; rendering = prior; Render(); }
    }
    private static Task<string> CreateEmptyAsync(string path) { File.WriteAllBytes(path, []); return Task.FromResult(path); }
    private async Task SaveCaptureAsync(string path, FrameworkElement? target = null)
    {
        var bitmap = new RenderTargetBitmap(); await bitmap.RenderAsync(target ?? navigation); var pixels = await bitmap.GetPixelsAsync();
        var file = await StorageFile.GetFileFromPathAsync(await CreateEmptyAsync(path)); using var stream = await file.OpenAsync(FileAccessMode.ReadWrite);
        var encoder = await BitmapEncoder.CreateAsync(BitmapEncoder.PngEncoderId, stream);
        encoder.SetPixelData(BitmapPixelFormat.Bgra8, BitmapAlphaMode.Premultiplied, (uint)bitmap.PixelWidth, (uint)bitmap.PixelHeight, 96, 96, pixels.ToArray()); await encoder.FlushAsync();
    }
}
