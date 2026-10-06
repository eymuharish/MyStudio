using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using System.IO;
using System.Text.Json;
using TeleStudio.Contracts;
using TeleStudio.Desktop;
using TeleStudio.Settings;
using Windows.UI;

namespace MyStudio.WinUI;

public sealed partial class MainWindow : Window
{
    private readonly NavigationView navigation = new() { PaneDisplayMode = NavigationViewPaneDisplayMode.Auto, IsSettingsVisible = false };
    private readonly StudioSession session = new();
    private readonly CancellationTokenSource lifetime = new();
    private readonly Dictionary<string, FrameworkElement> pages = [];
    private readonly TranslationBindings translations = new();
    private readonly Grid contentHost = new();
    private readonly InfoBar status = new() { IsOpen = false, IsClosable = true };
    private readonly TextBlock stateText = new() { FontSize = 20 };
    private readonly TextBlock diagnostics = new() { FontSize = 14, TextWrapping = TextWrapping.Wrap, FlowDirection = FlowDirection.LeftToRight };
    private readonly TextBlock mediaStatus = new() { TextWrapping = TextWrapping.Wrap };
    private readonly TextBox destination = new() { PlaceholderText = "10.136.51.25", FlowDirection = FlowDirection.LeftToRight };
    private readonly TextBox lecturer = new() { Text = "MyStudio" };
    private readonly ComboBox family = new() { ItemsSource = Enum.GetValues<EngineFamily>() };
    private readonly ComboBox protocol = new() { ItemsSource = Enum.GetValues<CallProtocol>() };
    private readonly ComboBox backend = new() { ItemsSource = new[] { SipEngineKind.Opal }, SelectedItem = SipEngineKind.Opal, IsEnabled = false };
    private readonly ComboBox endpoint = new() { ItemsSource = Enum.GetValues<EndpointProfile>() };
    private readonly ComboBox camera = new() { DisplayMemberPath = "Name" };
    private readonly ComboBox desktop = new() { DisplayMemberPath = "Name" };
    private readonly ComboBox resolution = new() { ItemsSource = Enum.GetValues<VideoResolution>(), SelectedItem = VideoResolution.Hd };
    private readonly NumberBox bitrate = new() { Minimum = 128, Maximum = 20000, Value = 1500, SpinButtonPlacementMode = NumberBoxSpinButtonPlacementMode.Compact };
    private readonly Image remoteImage = new() { Stretch = Stretch.Uniform };
    private readonly Image localImage = new() { Stretch = Stretch.Uniform, MaxHeight = 180 };
    private readonly Grid studioCards = new() { RowSpacing = 8, ColumnSpacing = 8 };
    private readonly ComboBox language = new() { ItemsSource = new[] { "العربية", "English" } };
    private readonly ComboBox theme = new() { ItemsSource = new[] { "System", "Light", "Dark" } };
    private Button? dial, answer, reject, end, showCallBar;
    private AppBarButton? microphone, room, shareFrom, stopSharing, systemAudio, audioTools;
    private readonly SharedVideoPresenter remotePresenter, localPresenter;
    private readonly Microsoft.UI.Xaml.DispatcherTimer videoTimer = new() { Interval = TimeSpan.FromMilliseconds(40) };
    private MediaCapabilitiesChanged? capabilities;
    private VideoStateChanged? videoState;
    private CaptureCatalogItem? sharedSource;
    private readonly OpalScreenSharingPause screenSharingPause = new();
    private RecordingStateChanged recording = new(RecordingState.Idle, false, false, 0, "");
    private bool arabic = false, rendering, busy, closing;
    private string transportEvidence = "—", audioEvidence = "—", contentEvidence = "—", registrationEvidence = "—";
    private string lastHealth = "—";
    private string callMode = "—", outboundEvidence = "—";
    private readonly List<object> eventEvidence = [];
    private FloatingCallWindow? callSidebar;
    private DateTimeOffset? connectedAt;
    private readonly TextBlock debugLog = new() { TextWrapping = TextWrapping.Wrap, FontFamily = new FontFamily("Consolas"), FontSize = 12, FlowDirection = FlowDirection.LeftToRight };
    private DateTimeOffset nextLogRefresh;
    public MainWindow()
    {
        Title = "MyStudio";
        AppWindow.SetIcon(Path.Combine(AppContext.BaseDirectory, "Assets", "EssaStudio.ico"));
        AppWindow.Resize(new Windows.Graphics.SizeInt32(1280, 820));
        if (Microsoft.UI.Composition.SystemBackdrops.MicaController.IsSupported()) SystemBackdrop = new MicaBackdrop();
        navigation.PaneTitle = "";
        navigation.Header = null;
        navigation.AlwaysShowHeader = false;
        remotePresenter = new(remoteImage); localPresenter = new(localImage);
        AddNavigation("studios", "الاستديوهات", "Studios", Symbol.Home);
        AddNavigation("dial", "اتصال مباشر", "Direct call", Symbol.Phone);
        AddNavigation("call", "المكالمة", "Active call", Symbol.Video);
        AddNavigation("recordings", "التسجيلات", "Recordings", Symbol.Save);
        AddNavigation("settings", "الإعدادات", "Settings", Symbol.Setting);
        AddNavigation("diagnostics", "Log and debug · السجل والتشخيص", "Log and debug", Symbol.Document);
        BuildPages();
        var wrapper = new Grid { Padding = new Thickness(24), RowSpacing = 16 };
        wrapper.RowDefinitions.Add(new() { Height = GridLength.Auto }); wrapper.RowDefinitions.Add(new() { Height = new GridLength(1, GridUnitType.Star) });
        wrapper.Children.Add(status); Grid.SetRow(contentHost, 1); wrapper.Children.Add(contentHost);
        navigation.Content = wrapper;
        wrapper.SizeChanged += (_, _) =>
        {
            var compact = wrapper.ActualHeight < 460;
            wrapper.Padding = new Thickness(compact ? 12 : 24); stateText.FontSize = compact ? 16 : 20;
            mediaStatus.FontSize = compact ? 10 : 12; mediaStatus.MaxLines = compact ? 1 : 2;
            if (pages["call"] is Grid layout) layout.RowSpacing = compact ? 4 : 10;
        };
        navigation.SelectionChanged += (_, args) => { if (args.SelectedItemContainer?.Tag is string page) ShowPage(page); };
        session.EventReceived += (_, value) =>
        {
            var emittingEngine = session.Engine;
            DispatcherQueue.TryEnqueue(() =>
            {
                if (ReferenceEquals(emittingEngine, session.Engine)) _ = ReceiveEventAsync(value);
                else if (value is RemoteSurfaceChanged remote) CloseUnpresentedHandle((nint)remote.HostHandleValue);
                else if (value is LocalSurfaceChanged local) CloseUnpresentedHandle((nint)local.HostHandleValue);
            });
        };
        videoTimer.Tick += (_, _) => { remotePresenter.Present(); localPresenter.Present(); UpdateCallSidebar(); RefreshDiagnostics(); };
        videoTimer.Start();
        family.SelectionChanged += async (_, _) => { if (!rendering && family.SelectedItem is EngineFamily next) await RunAsync(() => InitializeFamilyAsync(next)); };
        protocol.SelectionChanged += async (_, _) => { if (!rendering && protocol.SelectedItem is CallProtocol next) await RunAsync(async () => { await session.SelectProtocolAsync(next, lifetime.Token); Render(); }); };
        backend.SelectionChanged += async (_, _) => { if (!rendering && backend.SelectedItem is SipEngineKind next) await RunAsync(async () => { await session.SelectBackendAsync(next, lifetime.Token); Render(); }); };
        language.SelectionChanged += (_, _) => { if (!rendering) { arabic = language.SelectedIndex == 0; ApplyLanguage(); SaveAppearance(); } };
        theme.SelectionChanged += (_, _) => { if (!rendering) { ApplyTheme(); SaveAppearance(); } };
        AppWindow.Closing += async (_, args) => { if (closing) return; args.Cancel = true; await ShutdownAsync(); };
        Content = navigation;
        destination.KeyDown += async (_, key) => { if (key.Key == Windows.System.VirtualKey.Enter && session.Ready) { key.Handled = true; await RunAsync(DialAsync); } };
        destination.TextChanged += async (_, _) =>
        {
            Render();
            if (!rendering && !selectingStudio && !busy && session.Ready && session.Calls?.CanHangUp != true)
                await RunAsync(() => PrepareDestinationAsync());
        };
        navigation.KeyDown += async (_, key) =>
        {
            if (key.Key == Windows.System.VirtualKey.F11) { ToggleFullscreen(); key.Handled = true; }
            if (key.Key == Windows.System.VirtualKey.Escape && fullscreen) { ToggleFullscreen(); key.Handled = true; }
        };
        LoadAppearance(); ApplyLanguage(); ApplyTheme(); ShowPage("studios");
        wrapper.Loaded += async (_, _) => { ShowPage("studios"); await RunAsync(() => InitializeFamilyAsync(EngineFamily.Cisco)); if (await RunStudioTransitionSmokeIfRequestedAsync()) return; await RunLocalMediaSmokeIfRequestedAsync(); await RunUiSmokeIfRequestedAsync(); };
    }
    private Task ShutdownAsync() => ShutdownWithEvidenceAsync(null);
    private async Task ShutdownWithEvidenceAsync(List<string>? errors)
    {
        if (closing) return; closing = true; videoTimer.Stop(); StopAudioControls(); callSidebar?.Dispose();
        lifetime.Cancel();
        // Normal UI commands retire first. Final surface acknowledgments and native
        // recording/BYE completion retain independent shutdown budgets.
        try
        {
            using var surfaces = new CancellationTokenSource(OpalCallLifecycle.TerminationTimeout);
            await remotePresenter.DetachAsync(true, surfaces.Token); await localPresenter.DetachAsync(true, surfaces.Token);
        }
        catch (Exception error) { WriteLog("shutdown/surfaces", error.Message); errors?.Add("Surface shutdown: " + error); }
        try { await session.DisposeAsync(); }
        catch (Exception error) { WriteLog("shutdown/engine", error.Message); errors?.Add("Engine shutdown: " + error); }
        finally { lifetime.Cancel(); remotePresenter.Dispose(); localPresenter.Dispose(); Close(); }
    }
    private string T(string ar, string en) => arabic ? ar : en;
    private void AddNavigation(string key, string ar, string en, Symbol icon)
    {
        var item = new NavigationViewItem { Icon = new SymbolIcon(icon), Tag = key };
        translations.Add((value => item.Content = value, ar, en)); navigation.MenuItems.Add(item);
    }
    private void ShowPage(string key)
    {
        foreach (var page in pages) page.Value.Visibility = page.Key == key ? Visibility.Visible : Visibility.Collapsed;
        var item = navigation.MenuItems.Cast<NavigationViewItem>().First(x => (string)x.Tag == key);
        if (key == "recordings") RefreshRecordings();
        if (navigation.IsLoaded && !ReferenceEquals(navigation.SelectedItem, item)) navigation.SelectedItem = item;
    }
    private void ApplyLanguage()
    {
        var prior = rendering; rendering = true; language.SelectedIndex = arabic ? 0 : 1; rendering = prior;
        session.Calls?.SetLanguage(arabic ? TeleStudio.Presentation.AppLanguage.Arabic : TeleStudio.Presentation.AppLanguage.English);
        navigation.FlowDirection = arabic ? FlowDirection.RightToLeft : FlowDirection.LeftToRight;
        foreach (var item in translations) item.Set(T(item.Ar, item.En)); RefreshRecordings(); BuildStudioCards(); Render();
    }
    private void ApplyTheme() => navigation.RequestedTheme = theme.SelectedItem?.ToString() switch { "Light" => ElementTheme.Light, "Dark" => ElementTheme.Dark, _ => ElementTheme.Default };
    private void LoadAppearance(string fallbackLanguage = "en")
    {
        rendering = true;
        try
        {
            var path = Path.Combine(AppDataPaths.Root, "mystudio-appearance.json");
            var appearance = AppearancePreferences.Parse(File.Exists(path) ? File.ReadAllText(path) : null, fallbackLanguage);
            arabic = appearance.Language == "ar"; theme.SelectedItem = appearance.Theme;
            language.SelectedIndex = arabic ? 0 : 1;
        }
        catch { arabic = false; theme.SelectedItem = "System"; language.SelectedIndex = 1; }
        finally { rendering = false; }
    }
    private void SaveAppearance() => File.WriteAllText(Path.Combine(AppDataPaths.Root, "mystudio-appearance.json"), JsonSerializer.Serialize(new { language = arabic ? "ar" : "en", theme = theme.SelectedItem?.ToString() ?? "System" }));
    private async Task InitializeFamilyAsync(EngineFamily next)
    {
        StopAudioControls();
        await remotePresenter.DetachAsync(true, lifetime.Token); await localPresenter.DetachAsync(true, lifetime.Token);
        capabilities = null; videoState = null;
        try { await session.InitializeAsync(next, lifetime.Token); }
        catch { BuildStudioCards(); throw; }
        InitializeAudioControls();
        rendering = true;
        try
        {
            family.SelectedItem = next; protocol.SelectedItem = session.Calls!.SelectedProtocol;
            backend.SelectedItem = session.Engine!.SelectedSipEngine; endpoint.SelectedItem = session.Calls.SelectedEndpoint;
            lecturer.Text = session.Calls.LecturerName;
            resolution.SelectedItem = session.Settings.VideoResolution;
            bitrate.Value = session.Settings.VideoBitrateKbps ?? 2000;
            var captures = await session.Catalog!.RefreshAsync(lifetime.Token); desktop.ItemsSource = captures.Items.Where(x => x.Kind is CaptureItemKind.Monitor or CaptureItemKind.Window).ToArray(); desktop.SelectedIndex = desktop.Items.Count > 0 ? 0 : -1;
            var cameras = await session.Cameras!.RefreshAsync(lifetime.Token); camera.ItemsSource = cameras.Items;
            camera.SelectedItem = cameras.Items.FirstOrDefault(x => x.Id == session.Settings.CameraId) ?? cameras.Items.FirstOrDefault();
        }
        finally { rendering = false; }
        BuildStudioCards(); LoadAppearance(session.Settings.Language); ApplyLanguage(); RefreshRecentDestinations(); Render();
        session.Calls!.SetLanguage(arabic ? TeleStudio.Presentation.AppLanguage.Arabic : TeleStudio.Presentation.AppLanguage.English);
    }
    private async Task RunAsync(Func<Task> action)
    {
        if (busy) return; busy = true; Render();
        try { await action(); status.IsOpen = false; }
        catch (Exception error) { ReportError("action", error.Message); }
        finally { busy = false; Render(); }
    }
    private async Task ReceiveEventAsync(EngineEvent value)
    {
        if (closing) return;
        try
        {
            studioObservations.Apply(value);
            LogSupportEvent(value);
            if (value is CallFailed failure) { WriteLog("call/" + failure.ErrorCode, failure.Detail ?? failure.ErrorCode); nextLogRefresh = default; }
            if (value is IncomingCall or CallProgress or CallConnected or CallEnded or CallFailed or MediaCapabilitiesChanged or MicrophoneMuteChanged or MuteChanged or LocalSourceChanged or RemoteSurfaceChanged or MediaStatsChanged or AudioCodecStateChanged)
                session.Calls?.ApplyEvent(value);
            switch (value)
            {
                case IncomingCall: connectedAt = null; ResetCallEvidence(); ShowPage("call"); break;
                case CallConnected: connectedAt ??= DateTimeOffset.UtcNow; capabilities = null; ShowPage("call"); break;
                case MediaCapabilitiesChanged caps: capabilities = caps; break;
                case VideoStateChanged video: videoState = video; break;
                case MicrophoneMuteChanged state: micMuted = state.IsMuted; break;
                case RoomAudioMuteChanged state: roomMuted = state.IsMuted; break;
                case SystemAudioChanged state: systemEnabled = state.Enabled; break;
                case AudioControlsChanged state: confirmedAudio = state; break;
                case RoomMixerChanged state: confirmedMixer = state; break;
                case RecordingStateChanged state: recording = state; break;
                case AudioCodecStateChanged codec: audioEvidence = $"{codec.Negotiated} · {codec.SampleRate} Hz · {codec.Channels} ch · {codec.BitrateKbps} kbps · TX {codec.SendActive} / RX {codec.ReceiveActive}"; break;
                case SipSecurityStateChanged security: transportEvidence = $"{security.Transport} / {security.MediaEncryption}"; break;
                case SipCallModeChanged mode: callMode = mode.Mode; break;
                case SipAudioUnavailable unavailable: audioEvidence = T("الصوت غير متاح", "Audio unavailable") + ": " + unavailable.Detail; break;
                case SipOutboundVideoProofChanged proof: outboundEvidence = $"Encoded complete frames {proof.EncodedFrameEnds}; packets {proof.RtpPacketsSent}; peer RTCP report {proof.PeerReportAvailable}; reported sequence {proof.PeerExtendedSequence}; loss {proof.PeerLossPercent}%; SSRC {proof.SenderSsrc}/{proof.PeerReportedSsrc}. Remote display is unconfirmed."; break;
                case SipRegistrationStateChanged registration: registrationEvidence = registration.State + " " + registration.Detail; break;
                case ContentSharingStateChanged content: contentEvidence = $"{content.Mode} · active={content.Active} · {content.Detail}"; break;
                case SipConnectionHealthChanged health: lastHealth = $"Audio TX/RX {health.AudioTxKbps}/{health.AudioRxKbps} kbps · Video TX/RX {health.VideoTxKbps}/{health.VideoRxKbps} kbps\nLoss {health.LossPercent}% · jitter buffer {health.JitterBufferMs} ms · RTT {health.RoundTripMs} ms\nDecoded PCM {health.DecodedAudioSamples} · device submitted nonzero {health.DeviceSubmittedNonzeroSamples} · decoded video frames {health.ReceivedFrames}"; break;
                case RemoteSurfaceChanged surface when session.Engine is { } engine: await remotePresenter.AttachAsync(surface, engine.SendSurfaceAcknowledgementAsync, lifetime.Token); break;
                case LocalSurfaceChanged surface when session.Engine is { } engine: await localPresenter.AttachAsync(new RemoteSurfaceChanged(surface.HostHandleValue, surface.VideoWidth, surface.VideoHeight, surface.Epoch), engine.SendLocalSurfaceAcknowledgementAsync, lifetime.Token); break;
                case CallEnded or CallFailed: connectedAt = null; capabilities = null; videoState = null; sharedSource = null; ApplySharingCaptureExclusion(null); screenSharingPause.Clear(); await remotePresenter.DetachAsync(true, lifetime.Token); await localPresenter.DetachAsync(true, lifetime.Token); BuildStudioCards(); break;
                case MediaError error: ReportError("media/" + error.Code, error.Detail); break;
                case RecordingError error: ReportError("recording/" + error.Code, error.Detail); break;
            }
            ApplyAudioControlEvent(value);
            eventEvidence.Add(value); if (eventEvidence.Count > 1000) eventEvidence.RemoveAt(0);
            Render();
        }
        catch (Exception error) { ReportError("event", error.Message); }
    }
    private void Render()
    {
        var calls = session.Calls; var connected = calls?.IsConnected == true;
        var policy = StudioControlPolicy.From(connected, capabilities, recording.State, OpalRecordingPolicy.IsSharedDesktopActive(videoState, sharedSource));
        var inCall = calls?.CanHangUp == true;
        if (inCall) status.IsOpen = false;
        status.Visibility = inCall ? Visibility.Collapsed : Visibility.Visible;
        var idle = !inCall && !busy;
        var priorRendering = rendering; rendering = true;
        family.SelectedItem = session.Family;
        var allowedProtocols = session.SupportedStudios.AllowedProtocols(destination.Text);
        if (!protocol.Items.Cast<CallProtocol>().SequenceEqual(allowedProtocols)) protocol.ItemsSource = allowedProtocols;
        if (calls is not null) protocol.SelectedItem = allowedProtocols.Contains(calls.SelectedProtocol) ? calls.SelectedProtocol : null;
        backend.SelectedItem = session.Engine?.SelectedSipEngine ?? session.Settings.SipEngine;
        rendering = priorRendering;
        var recognizedStudio = session.SupportedStudios.Find(destination.Text);
        family.IsEnabled = idle && recognizedStudio is null; protocol.IsEnabled = idle && allowedProtocols.Length > 0; backend.IsEnabled = false;
        endpoint.IsEnabled = idle && recognizedStudio is null;
        studioSupportNotes.Text = session.SupportedStudios.Notes(destination.Text, arabic);
        if (dial is not null) dial.IsEnabled = session.Ready && idle && calls is not null &&
            session.SupportedStudios.Allows(destination.Text, calls.SelectedProtocol, session.Tuning.AacEnabled);
        if (redial is not null) redial.IsEnabled = session.Ready && idle && recentDestinations.Items.Count > 0;
        recentDestinations.IsEnabled = idle;
        foreach (var button in studioIdleButtons) button.IsEnabled = session.Ready && idle;
        if (studioAddButton is not null) studioAddButton.IsEnabled = session.Ready && idle;
        if (answer is not null) answer.IsEnabled = !busy && calls?.CanAnswer == true;
        if (reject is not null) reject.IsEnabled = !busy && calls?.CanReject == true;
        if (end is not null) end.IsEnabled = !busy && calls?.CanHangUp == true;
        if (microphone is not null) microphone.IsEnabled = callAudio?.CanToggleMicrophoneMute == true && !busy;
        if (room is not null) room.IsEnabled = callAudio?.CanToggleRoomMute == true && !busy;
        var sharing = OpalSharingPolicy.Controls(connected, policy.Camera, busy, videoState);
        if (shareFrom is not null) shareFrom.IsEnabled = sharing.CanChoose;
        if (stopSharing is not null) { stopSharing.IsEnabled = sharing.CanStop; stopSharing.Visibility = sharing.CanStop || sharing.Active ? Visibility.Visible : Visibility.Collapsed; }
        if (systemAudio is not null) systemAudio.IsEnabled = callAudio?.CanToggleSystemAudioMute == true && !busy;
        if (audioTools is not null) audioTools.IsEnabled = policy.Audio && !busy;
        stateText.Text = calls?.Session.State switch
        {
            CallState.Idle => T("جاهز للاتصال", "Ready to call"), CallState.Incoming => T("مكالمة واردة", "Incoming call"),
            CallState.Calling => T("جارٍ الاتصال", "Calling"), CallState.Negotiating => T("جارٍ التفاوض", "Negotiating"),
            CallState.ConnectedCamera => T("المكالمة متصلة", "Call connected"), CallState.ConnectedDesktop => T("المكالمة متصلة · مشاركة الشاشة", "Call connected · Screen sharing"),
            CallState.Ending => T("جارٍ إنهاء المكالمة", "Ending call"), CallState.Failed => T("فشل الاتصال", "Call failed"), _ => T("المحرك غير جاهز", "Engine is not ready")
        };
        RenderCallToolStates();
        mediaStatus.Text = connected ? $"{audioEvidence} · {videoState?.AppliedWidth}×{videoState?.AppliedHeight} · {videoState?.AppliedBitrateKbps} kbps\n{T("مشاركة المحتوى", "Content sharing")}: {contentEvidence}" : inCall ? T("بانتظار تأكيد الوسائط", "Waiting for media confirmation") : T("لا توجد مكالمة نشطة", "No active call");
        if (showCallBar is not null) showCallBar.IsEnabled = calls?.CanHangUp == true;
        UpdateCallSidebar(); RefreshDiagnostics();
    }
    private void RefreshDiagnostics()
    {
        if (DateTimeOffset.UtcNow >= nextLogRefresh)
        {
            nextLogRefresh = DateTimeOffset.UtcNow.AddSeconds(1);
            var persisted = ReadMyStudioLog();
            debugLog.Text = persisted.Length <= 24000 ? persisted : persisted[^24000..];
        }
        diagnostics.Text = $"WinUI 3 · {session.Family}\nAAC plugin: {session.Engine?.AacAvailability.Detail}\n{(session.Calls?.IsConnected == true ? "Current call media" : "Last call media / no active call")}\nSIP registration: {registrationEvidence}\nCall mode: {callMode}\nNegotiated security: {transportEvidence}\nNegotiated audio: {audioEvidence}\nContent: {contentEvidence}\n{lastHealth}\nOutbound video: {outboundEvidence}\nPresenter: {remotePresenter.Mode}\nRemote frames presented: {remotePresenter.PresentedFrames} · CPU copy {remotePresenter.LastCopyMilliseconds:F2} ms\nLocal frames presented: {localPresenter.PresentedFrames}\n{remotePresenter.Error}";
    }
    private void ResetCallEvidence() { connectedAt = null; sharedSource = null; screenSharingPause.Clear(); videoState = null; audioEvidence = transportEvidence = contentEvidence = lastHealth = outboundEvidence = callMode = "—"; micMuted = roomMuted = systemEnabled = false; }
    private static string ReadMyStudioLog(bool full = false) => TeleStudio.Desktop.Runtime.DiagnosticLog.ReadSnapshot(full ? 4 * 1024 * 1024 : 24000)
        .Replace("EssaStudio diagnostics / سجل التشخيص", "Log and debug / سجل التشخيص", StringComparison.Ordinal)
        .Replace($"Version: {typeof(TeleStudio.Desktop.Runtime.DiagnosticLog).Assembly.GetName().Version};",
            $"Version: {typeof(MainWindow).Assembly.GetName().Version};", StringComparison.Ordinal);
    private static void WriteLog(string area, string message) => TeleStudio.Desktop.Runtime.DiagnosticLog.Write(area, message);
    private void ReportError(string area, string message)
    {
        WriteLog(area, message); nextLogRefresh = default;
        if (session.Calls?.CanHangUp == true) { status.IsOpen = false; return; }
        status.IsOpen = true; status.Severity = InfoBarSeverity.Error;
        status.Message = TeleStudio.Desktop.Runtime.DiagnosticLog.Redact(message);
    }
    private void UpdateCallSidebar()
    {
        var active = session.Calls?.CanHangUp == true;
        if (active && callSidebar is null)
            callSidebar = new(this, () => RunAsync(ToggleMicrophoneAsync),
                () => RunAsync(ToggleRoomAudioAsync),
                () => RunAsync(ToggleSystemAudioAsync),
                () => RunAsync(ToggleScreenSharingPauseAsync), () => RunAsync(ShowRecordingAsync),
                () => RunAsync(() => SetSharedRecordingPausedAsync(!recording.Paused)),
                () => RunAsync(() => SendAsync(new StopLectureRecording())),
                () => RunAsync(() => session.Calls!.HangUpAsync(lifetime.Token)));
        callSidebar?.Update(active, stateText.Text, connectedAt is { } start && active ? DateTimeOffset.UtcNow - start : TimeSpan.Zero,
            recording, CanStartSharedRecording, busy, arabic, micMuted, microphone?.IsEnabled == true,
            roomMuted, room?.IsEnabled == true, systemEnabled, systemAudio?.IsEnabled == true, CanPauseOrResumeSharedRecording,
            ScreenSharingPauseControls,
            callAudio?.IsSystemAudioPending == true ? !callAudio.PendingSystemAudioMuted : null);
    }
    private ScreenSharingPauseView ScreenSharingPauseControls
    {
        get => GetScreenSharingPauseControls(busy);
    }
    private ScreenSharingPauseView GetScreenSharingPauseControls(bool isBusy)
    {
        screenSharingPause.ConfirmResume(videoState, sharedSource);
        return screenSharingPause.Controls(session.Calls?.IsConnected == true, capabilities is { SourceControl: true, VideoControl: true },
            isBusy, session.Engine?.ConfirmedHeld == true, videoState, sharedSource);
    }
    private bool CanStartSharedRecording => StudioControlPolicy.From(session.Calls?.IsConnected == true, capabilities,
        recording.State, OpalRecordingPolicy.IsSharedDesktopActive(videoState, sharedSource)).Record;
    private Task StartSidebarRecordingAsync() => SendAsync(OpalRecordingPolicy.Start(CanStartSharedRecording, videoState, sharedSource));
    private bool CanPauseOrResumeSharedRecording => OpalRecordingPolicy.CanPauseOrResume(recording, videoState, sharedSource);
    private Task SetSharedRecordingPausedAsync(bool paused) => SendAsync(OpalRecordingPolicy.SetPaused(recording, paused, videoState, sharedSource));
    [System.Runtime.InteropServices.DllImport("kernel32", EntryPoint = "CloseHandle")] private static extern bool CloseUnpresentedHandle(nint handle);
}

