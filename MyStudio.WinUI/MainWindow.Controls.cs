using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using TeleStudio.Contracts;
using TeleStudio.Settings;
using TeleStudio.Desktop;

namespace MyStudio.WinUI;

public sealed partial class MainWindow
{
    private readonly Dictionary<string, FrameworkElement> tools = [];
    private readonly TextBox studioSearch = new();
    private readonly List<Button> studioIdleButtons = [];
    private Button? studioAddButton;
    private readonly TextBlock studioSupportNotes = new() { TextWrapping = TextWrapping.Wrap, FontSize = 13 };
    private string? preparedStudioId;
    private bool selectingStudio;
    private readonly SemaphoreSlim destinationPreparationGate = new(1, 1);
    private bool micMuted, roomMuted, systemEnabled;
    private static StackPanel Stack() => new() { Spacing = 14, MaxWidth = 1040, HorizontalAlignment = HorizontalAlignment.Stretch };
    private TextBlock Text(string ar, string en, double size = 14)
    {
        var text = new TextBlock { FontSize = size, TextWrapping = TextWrapping.Wrap };
        translations.Add((value => text.Text = value, ar, en)); return text;
    }
    private Button Action(string ar, string en, Func<Task> action)
    {
        var button = new Button { MinHeight = 40 }; translations.Add((value => button.Content = value, ar, en));
        button.Click += async (_, _) => await RunAsync(action); return button;
    }
    private void Field(StackPanel panel, string ar, string en, FrameworkElement field)
    { panel.Children.Add(Text(ar, en)); field.HorizontalAlignment = HorizontalAlignment.Stretch; panel.Children.Add(field); }
    private void Page(string key, StackPanel body)
    { var scroll = CenteredPage(body); pages.Add(key, scroll); contentHost.Children.Add(scroll); }
    private static ScrollViewer CenteredPage(FrameworkElement body)
    {
        body.HorizontalAlignment = HorizontalAlignment.Center;
        var scroll = new ScrollViewer { Content = body, Padding = new Thickness(0, 0, 0, 24),
            HorizontalContentAlignment = HorizontalAlignment.Center, HorizontalScrollMode = ScrollMode.Disabled,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled };
        void Fit() { if (scroll.ActualWidth > 0) body.Width = Math.Min(1040, Math.Max(0, scroll.ActualWidth - 20)); }
        scroll.Loaded += (_, _) => Fit(); scroll.SizeChanged += (_, _) => Fit();
        return scroll;
    }
    private void BuildPages()
    {
        var studios = Stack();
        var studioToolbar = new Grid { ColumnSpacing = 12 };
        studioToolbar.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        studioToolbar.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
        studioSearch.HorizontalAlignment = HorizontalAlignment.Stretch;
        ToolTipService.SetToolTip(studioSearch, T("البحث بالاسم أو العنوان", "Search by name or address"));
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(studioSearch, "Search studios");
        studioSearch.TextChanged += (_, _) => BuildStudioCards(); studioToolbar.Children.Add(studioSearch);
        var addStudio = StudioIcon(Symbol.Add, "إضافة استديو", "Add studio", () => EditStudioAsync(null));
        studioAddButton = addStudio;
        translations.Add((value => { ToolTipService.SetToolTip(addStudio, value); Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(addStudio, value); }, "إضافة استديو", "Add studio"));
        translations.Add((value => { ToolTipService.SetToolTip(studioSearch, value); Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(studioSearch, value); }, "البحث بالاسم أو العنوان", "Search by name or address"));
        Grid.SetColumn(addStudio, 1); studioToolbar.Children.Add(addStudio);
        studios.Children.Add(studioToolbar); studios.Children.Add(studioCards); Page("studios", studios);
        studioCards.SizeChanged += (_, _) => ArrangeStudioCards();
        var direct = Stack(); direct.Children.Add(Text("اتصال مباشر", "Direct call", 30));
        Field(direct, "عنوان الجهاز أو SIP URI", "Device address or SIP URI", destination);
        dial = Action("اتصال", "Call", DialAsync); direct.Children.Add(dial);
        Field(direct, "العناوين الأخيرة", "Recent destinations", recentDestinations);
        recentDestinations.SelectionChanged += (_, _) => { if (recentDestinations.SelectedItem is string address) destination.Text = address; };
        redial = Action("إعادة الاتصال بآخر عنوان", "Redial last destination", async () => { if (recentDestinations.Items.Count == 0) return; destination.Text = recentDestinations.Items[0].ToString(); await DialAsync(); }); direct.Children.Add(redial);
        var advanced = Stack(); Field(advanced, "عائلة الجهاز", "Device family", family); Field(advanced, "البروتوكول", "Protocol", protocol);
        Field(advanced, "محرك SIP", "SIP engine", backend); Field(advanced, "الجهاز", "Endpoint profile", endpoint);
        Field(advanced, "اسمك لدى الطرف الآخر", "Your display name", lecturer);
        var advancedOptions = new Expander { Content = advanced, IsExpanded = false, HorizontalAlignment = HorizontalAlignment.Stretch };
        translations.Add((value => advancedOptions.Header = value, "خيارات الاتصال المتقدمة", "Advanced connection options")); direct.Children.Add(advancedOptions); Page("dial", direct);
        var active = Stack(); active.Children.Add(Text("المكالمة", "Active call", 30)); active.Children.Add(stateText);
        var video = new Grid { MinHeight = 260, MaxHeight = 420, Background = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 14, 22, 32)) };
        video.Children.Add(remoteImage); localImage.HorizontalAlignment = HorizontalAlignment.Right; localImage.VerticalAlignment = VerticalAlignment.Bottom; localImage.Width = 240; video.Children.Add(localImage); active.Children.Add(video);
        var commands = new CommandBar { DefaultLabelPosition = CommandBarDefaultLabelPosition.Right, IsDynamicOverflowEnabled = true };
        microphone = Tool("الميكروفون", "Microphone", Symbol.Microphone, ToggleMicrophoneAsync);
        room = Tool("صوت الغرفة", "Room audio", Symbol.Volume, ToggleRoomAudioAsync);
        shareFrom = Tool("مشاركة من", "Share from", Symbol.Share, ShowSharingSourceAsync);
        stopSharing = Tool("إيقاف المشاركة", "Stop sharing", Symbol.Stop, StopSharingAsync);
        systemAudio = Tool("صوت النظام", "System audio", Symbol.Audio, ToggleSystemAudioAsync);
        audioTools = Tool("ضبط الصوت", "Audio controls", Symbol.Setting, ShowAudioAsync);
        foreach (var tool in new[] { microphone, room, shareFrom, stopSharing, systemAudio, audioTools }) commands.PrimaryCommands.Add(tool);
        commands.SecondaryCommands.Add(Tool("الأجهزة", "Devices", Symbol.Setting, ShowDevicesAsync));
        commands.SecondaryCommands.Add(Tool("ضبط الفيديو", "Video controls", Symbol.Video, ShowVideoAsync)); active.Children.Add(commands);
        commands.SecondaryCommands.Add(Tool("ملء الشاشة", "Fullscreen", Symbol.FullScreen, () => { ToggleFullscreen(); return Task.CompletedTask; }));
        var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        answer = Action("قبول", "Answer", () => session.Calls!.AnswerAsync(lifetime.Token)); reject = Action("رفض", "Reject", () => session.Calls!.RejectAsync(lifetime.Token));
        end = Action("إنهاء", "End call", () => session.Calls!.HangUpAsync(lifetime.Token));
        end.Background = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 155, 35, 45));
        end.Foreground = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 255, 255, 255));
        showCallBar = Action("شريط المكالمة", "Call bar", () => { callSidebar?.Reopen(); return Task.CompletedTask; });
        foreach (var button in new[] { answer, reject, end, showCallBar }) actions.Children.Add(button); active.Children.Add(actions);
        active.Children.Clear(); video.MinHeight = 120; video.MaxHeight = double.PositiveInfinity;
        var callLayout = new Grid { RowSpacing = 10 };
        foreach (var height in new[] { GridLength.Auto, GridLength.Auto, GridLength.Auto, new GridLength(1, GridUnitType.Star) }) callLayout.RowDefinitions.Add(new() { Height = height });
        void Place(FrameworkElement element, int row) { Grid.SetRow(element, row); callLayout.Children.Add(element); }
        var headline = new StackPanel { Spacing = 4 }; headline.Children.Add(stateText); mediaStatus.FontSize = 12; mediaStatus.MaxLines = 2; headline.Children.Add(mediaStatus);
        Place(headline, 0); Place(actions, 1); Place(commands, 2); Place(video, 3);
        pages.Add("call", callLayout); contentHost.Children.Add(callLayout);
        BuildRecordingsPage();
        var settings = Stack(); settings.Children.Add(Text("الإعدادات", "Settings", 30)); Field(settings, "اللغة", "Language", language); Field(settings, "المظهر", "Appearance", theme);
        settings.Children.Add(Action("أجهزة الصوت والاسم", "Audio devices and display name", ShowDevicesAsync));
        settings.Children.Add(Action("الصوت والشبكة", "Audio and network tuning", ShowConnectionAsync));
        settings.Children.Add(Action("مصادر الفيديو والجودة", "Video sources and quality", ShowVideoAsync));
        settings.Children.Add(Text("الإصدار v2.0.2", "Version v2.0.2"));
        settings.Children.Add(Text("تُفعّل الأدوات بعد تأكيد المحرك. يفضّل AAC الاستريو مع بديل مونو أو G.722.", "Tools activate after engine confirmation. AAC stereo is preferred with mono or G.722 fallback.")); Page("settings", settings);
        settings.Children.Add(Text(TeleStudio.Presentation.MainViewModel.RightsStatement, "Copyright reserved to the application designer: الأستاذ. عيسى يحي مهارش."));
        var debug = new Grid { ColumnSpacing = 20, FlowDirection = FlowDirection.LeftToRight };
        debug.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        debug.ColumnDefinitions.Add(new() { Width = new GridLength(1.5, GridUnitType.Star) });
        var live = Stack(); live.Children.Add(Text("التشخيص المباشر", "Live diagnostics", 24)); live.Children.Add(diagnostics);
        live.Children.Add(Action("تصدير السجل والتشخيص", "Export log and debug", ExportDiagnosticsAsync));
        var persisted = Stack(); persisted.Children.Add(Text("Log and debug · السجل والتشخيص", "Log and debug", 24)); persisted.Children.Add(Text("تُحفظ الأخطاء هنا دون مقاطعة المكالمة", "Errors are saved here without interrupting the call")); persisted.Children.Add(debugLog);
        debug.Children.Add(new ScrollViewer { Content = live }); var right = new ScrollViewer { Content = persisted }; Grid.SetColumn(right, 1); debug.Children.Add(right);
        var debugPage = CenteredPage(debug); pages.Add("diagnostics", debugPage); contentHost.Children.Add(debugPage);
    }
    private AppBarButton Tool(string ar, string en, Symbol symbol, Func<Task> action)
    {
        var tool = new AppBarButton { Icon = new SymbolIcon(symbol) }; translations.Add((value => tool.Label = value, ar, en));
        tool.Click += async (_, _) => await RunAsync(action); return tool;
    }
    private Task SendAsync(EngineCommand command) => session.Engine?.SendAsync(command, lifetime.Token) ?? throw new InvalidOperationException("Engine is not ready.");
    private bool fullscreen;
    private void ToggleFullscreen() { fullscreen = !fullscreen; AppWindow.SetPresenter(fullscreen ? Microsoft.UI.Windowing.AppWindowPresenterKind.FullScreen : Microsoft.UI.Windowing.AppWindowPresenterKind.Overlapped); }
    private async Task DialAsync()
    {
        session.RequireIdle(); await PrepareDestinationAsync();
        var supported = session.SupportedStudios.RequireAllowed(destination.Text.Trim(), session.Calls!.SelectedProtocol, session.Tuning.AacEnabled,
            session.Tuning.AudioChannels, session.Tuning.AudioKbps);
        var address = supported?.Address ?? destination.Text.Trim();
        ResetCallEvidence(); var calls = session.Calls!; calls.Destination = address;
        calls.SelectedEndpoint = (EndpointProfile)endpoint.SelectedItem; calls.SelectedNetworkMode = session.ConnectionMode;
        if (!calls.TryAcceptLecturerName(lecturer.Text, out var error)) throw new ArgumentException(error);
        studioObservations.Begin(address, calls.SelectedProtocol);
        try { await calls.DialAsync(lifetime.Token); }
        catch { studioObservations.Apply(new CallFailed("local-dial-error")); throw; }
        await session.RememberDestinationAsync(address, lifetime.Token); RefreshRecentDestinations(); ShowPage("call");
    }
    private async Task PrepareDestinationAsync(bool recommend = false)
    {
        await destinationPreparationGate.WaitAsync(lifetime.Token);
        try { await PrepareDestinationCoreAsync(recommend); }
        finally { destinationPreparationGate.Release(); }
    }
    private async Task PrepareDestinationCoreAsync(bool recommend)
    {
        session.RequireIdle(); session.TargetDestination = destination.Text.Trim();
        if (session.SupportedStudios.Find(session.TargetDestination) is not { } studio) { preparedStudioId = null; return; }
        var changed = preparedStudioId != studio.Id;
        if (session.Family != studio.Family)
        {
            var displayName = lecturer.Text;
            await InitializeFamilyAsync(studio.Family);
            lecturer.Text = displayName;
        }
        await session.PrepareStudioAsync(studio, recommend || changed, lifetime.Token);
        endpoint.SelectedItem = studio.EndpointProfile; preparedStudioId = studio.Id;
    }
    private async Task SelectStudioAsync(RoomDirectoryEntry entry)
    {
        selectingStudio = true;
        var priorRendering = rendering; rendering = true;
        try { destination.Text = entry.Destination; }
        finally { rendering = priorRendering; }
        try
        {
            if (session.SupportedStudios.Find(entry.Destination) is not null) await PrepareDestinationAsync(recommend: true);
            else
            {
                var expected = entry.EndpointProfile is EndpointProfile.SonyXg77 or EndpointProfile.SonyXg80 ? EngineFamily.Sony : EngineFamily.Cisco;
                if (session.Family != expected) await InitializeFamilyAsync(expected);
                endpoint.SelectedItem = entry.EndpointProfile; session.TargetDestination = entry.Destination;
            }
            ShowPage("dial");
            // Deliver queued text notifications while automatic preparation is suppressed.
            await Task.Yield();
        }
        finally { selectingStudio = false; }
    }
    private Button StudioIcon(Symbol symbol, string ar, string en, Func<Task> action, string? studioName = null, bool idleOnly = false)
    {
        var label = T(ar, en);
        var button = new Button { Content = new SymbolIcon(symbol), Width = 24, Height = 24, MinWidth = 0, MinHeight = 0, Padding = new Thickness(1) };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(button, label + (studioName is null ? "" : " · " + studioName));
        ToolTipService.SetToolTip(button, label);
        button.Click += async (_, _) => await RunAsync(action);
        if (idleOnly) studioIdleButtons.Add(button);
        return button;
    }
    private void ArrangeStudioCards()
    {
        var columns = studioCards.ActualWidth >= 650 ? 5 : studioCards.ActualWidth >= 440 ? 4 : studioCards.ActualWidth >= 340 ? 3 : 2;
        if (studioCards.ColumnDefinitions.Count != columns)
        {
            studioCards.ColumnDefinitions.Clear();
            for (var i = 0; i < columns; i++) studioCards.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        }
        studioCards.RowDefinitions.Clear();
        for (var i = 0; i < (studioCards.Children.Count + columns - 1) / columns; i++) studioCards.RowDefinitions.Add(new() { Height = GridLength.Auto });
        for (var i = 0; i < studioCards.Children.Count; i++) { var card = (FrameworkElement)studioCards.Children[i]; Grid.SetRow(card, i / columns); Grid.SetColumn(card, i % columns); }
    }
    private void BuildStudioCards()
    {
        studioCards.Children.Clear(); studioIdleButtons.Clear();
        foreach (var entry in RoomDirectoryStore.Search(session.Rooms, studioSearch.Text))
        {
            var card = new StackPanel { Spacing = 2, HorizontalAlignment = HorizontalAlignment.Stretch };
            var name = new TextBlock { Text = entry.Name, FontSize = 16, HorizontalAlignment = HorizontalAlignment.Stretch,
                TextAlignment = TextAlignment.Center, TextTrimming = TextTrimming.CharacterEllipsis, MaxLines = 1 };
            ToolTipService.SetToolTip(name, entry.Name); card.Children.Add(name);
            var actions = new Grid { HorizontalAlignment = HorizontalAlignment.Center, ColumnSpacing = 4, RowSpacing = 4 };
            for (var i = 0; i < 3; i++) actions.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
            actions.RowDefinitions.Add(new() { Height = GridLength.Auto }); actions.RowDefinitions.Add(new() { Height = GridLength.Auto });
            var call = StudioIcon(Symbol.Phone, "اتصال مباشر", "Call studio", async () => { session.RequireIdle(); await SelectStudioAsync(entry); await DialAsync(); }, entry.Name, idleOnly: true);
            call.Background = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 0, 90, 158));
            call.Foreground = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 255, 255, 255));
            var buttons = new[] {
                call,
                StudioIcon(Symbol.Accept, "اختيار", "Select", () => SelectStudioAsync(entry), entry.Name, idleOnly: true),
                StudioIcon(Symbol.Edit, "تعديل", "Edit", () => EditStudioAsync(entry), entry.Name, idleOnly: true),
                StudioIcon(Symbol.Globe, "واجهة الجهاز", "Device web", async () => { if (!await Windows.System.Launcher.LaunchUriAsync(new Uri("http://" + entry.Destination))) throw new InvalidOperationException("Could not open device web interface."); }, entry.Name),
                StudioIcon(studioObservations.Find(entry.Destination)?.State == "failed" ? Symbol.Important : Symbol.Help,
                    "ملاحظات الدعم وآخر حالة", "Support notes and latest status", () => ShowStudioSupportAsync(entry), entry.Name),
                StudioIcon(Symbol.Delete, "حذف من القائمة", "Remove from list", () => RemoveStudioAsync(entry), entry.Name, idleOnly: true)
            };
            for (var i = 0; i < buttons.Length; i++) { Grid.SetRow(buttons[i], i / 3); Grid.SetColumn(buttons[i], i % 3); actions.Children.Add(buttons[i]); }
            card.Children.Add(actions);
            studioCards.Children.Add(new Border { Padding = new Thickness(4), CornerRadius = new CornerRadius(12), BorderThickness = new Thickness(1),
                BorderBrush = new SolidColorBrush(Windows.UI.Color.FromArgb(55, 128, 128, 128)), Child = card });
        }
        ArrangeStudioCards();
        foreach (var item in translations) item.Set(T(item.Ar, item.En));
        Render();
    }
}
