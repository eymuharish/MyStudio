using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using TeleStudio.Contracts;
using TeleStudio.Settings;
using TeleStudio.Desktop;
using TeleStudio.Desktop.Runtime;
using NAudio.CoreAudioApi;
using System.IO;

namespace MyStudio.WinUI;
public sealed partial class MainWindow
{
    private string? dialogCaptureFolder;
    private async Task<bool> DialogAsync(string ar, string en, StackPanel content, Action<ContentDialog>? configure = null)
    {
        var dialog = new ContentDialog { Title = T(ar, en), Content = new ScrollViewer { Content = content, MaxHeight = 470 }, XamlRoot = navigation.XamlRoot,
            PrimaryButtonText = T("تطبيق", "Apply"), CloseButtonText = T("إلغاء", "Cancel"), DefaultButton = ContentDialogButton.Primary, RequestedTheme = navigation.ActualTheme,
            FlowDirection = navigation.FlowDirection };
        configure?.Invoke(dialog);
        foreach (var item in translations) item.Set(T(item.Ar, item.En));
        Exception? captureError = null;
        if (dialogCaptureFolder is { } captureFolder)
            dialog.Opened += async (_, _) =>
            {
                var applied = false;
                try
                {
                    await Task.Delay(180);
                    var audioCheck = en == "Audio controls" && Environment.GetCommandLineArgs().Contains("--room-audio-controls-check");
                    if (audioCheck) VerifyRoomAudioControlsForSmoke(content);
                    if (en == "Share from" && Environment.GetCommandLineArgs().Contains("--sharing-transmission-check")) VerifySharingTransmissionDialogForSmoke(dialog, content);
                    await SaveCaptureAsync(Path.Combine(captureFolder, "dialog-" + en.ToLowerInvariant().Replace(' ', '-') + ".png"), dialog);
                    if (audioCheck) { ApplyRoomAudioDialogForSmoke(dialog); applied = true; }
                }
                catch (Exception error) { captureError = error; }
                finally { if (!applied) dialog.Hide(); }
            };
        var result = await dialog.ShowAsync();
        if (captureError is not null) throw captureError;
        return result == ContentDialogResult.Primary;
    }
    private NumberBox Number(double value, double min, double max) => new() { Value = value, Minimum = min, Maximum = max, SpinButtonPlacementMode = NumberBoxSpinButtonPlacementMode.Compact };
    private ComboBox Choice<TValue>(IEnumerable<TValue> items, TValue current) => new() { ItemsSource = items.ToArray(), SelectedItem = current, HorizontalAlignment = HorizontalAlignment.Stretch };
    private async Task ShowAudioAsync()
    {
        using var dialogTranslations = translations.BeginScope();
        if (capabilities?.AudioControl != true) throw new InvalidOperationException(T("التحكم بالصوت غير متاح بعد", "Audio control has not been confirmed."));
        var panel = Stack();
        var mixerAvailable = capabilities.RoomMixerControl;
        var volume = Number(mixerAvailable ? confirmedMixer.MasterPercent : confirmedAudio.RoomVolume, 0, mixerAvailable ? 300 : 100);
        volume.Name = "RoomVolume";
        var gain = Number(confirmedAudio.MicrophoneGain, 0, 150); gain.Name = "MicrophoneGain";
        panel.Children.Add(Text("صوت القاعة على جهازك", "Incoming room audio on this device", 18));
        Field(panel, "مستوى الصوت %", "Volume %", volume);
        var bass = Number(confirmedMixer.BassDb, -12, 12); bass.Name = "RoomBass";
        var voice = Number(confirmedMixer.VoiceDb, -12, 12); voice.Name = "RoomVoice";
        var treble = Number(confirmedMixer.TrebleDb, -12, 12); treble.Name = "RoomTreble";
        var reduction = Number(confirmedMixer.NoiseReductionDb, 0, 30);
        if (mixerAvailable)
        {
            panel.Children.Add(Text("إكولايزر — محدّد التشوّه يعمل تلقائياً عند رفع الصوت.", "Equalizer — automatic limiting protects boosted audio."));
            var presets = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
            void Preset(string name, string ar, string en, int level, int low, int speech, int high)
            {
                var button = Action(ar, en, () =>
                {
                    volume.Value = level; bass.Value = low; voice.Value = speech; treble.Value = high; reduction.Value = 0;
                    return Task.CompletedTask;
                });
                button.Name = name; presets.Children.Add(button);
            }
            Preset("RoomPresetFlat", "متوازن", "Balanced", 100, 0, 0, 0);
            Preset("RoomPresetVoice", "وضوح الكلام", "Clear speech", 150, -3, 4, 2);
            Preset("RoomPresetWeak", "سماعات ضعيفة", "Weak speakers", 200, -4, 3, 1);
            panel.Children.Add(presets);
            var bands = new Grid { ColumnSpacing = 10 };
            for (var i = 0; i < 3; i++) bands.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            var lows = Stack(); var speech = Stack(); var highs = Stack();
            Field(lows, "الجهير dB", "Bass dB", bass); Field(speech, "وضوح الكلام dB", "Speech dB", voice); Field(highs, "الترددات العالية dB", "Treble dB", treble);
            Grid.SetColumn(speech, 1); Grid.SetColumn(highs, 2);
            bands.Children.Add(lows); bands.Children.Add(speech); bands.Children.Add(highs); panel.Children.Add(bands);
            Field(panel, "تقليل ضوضاء القاعة dB", "Room noise reduction dB", reduction);
        }
        panel.Children.Add(Text("الميكروفون", "Microphone", 18));
        var noise = new CheckBox { Content = T("عزل ضوضاء الميكروفون", "Microphone noise isolation"), IsChecked = confirmedAudio.NoiseIsolation };
        var echoState = session.Engine!.EchoState;
        var echo = new CheckBox { Content = T("إلغاء الصدى بين السماعة والميكروفون", "Speaker/microphone echo cancellation"), IsChecked = echoState.Enabled, IsEnabled = echoState.Available };
        Field(panel, "كسب الميكروفون %", "Microphone gain %", gain); panel.Children.Add(noise);
        panel.Children.Add(echo);
        if (!await DialogAsync("ضبط الصوت", "Audio controls", panel)) return;
        // The legacy volume command accepts 0..100. The confirmed mixer carries
        // the full incoming gain, so send it last without truncating the boost.
        await SendAsync(new SetAudioControls((int)Math.Min(volume.Value, 100), (int)gain.Value, noise.IsChecked == true));
        if (echoState.Available) await SendAsync(new SetEchoCancellation(echo.IsChecked == true));
        if (mixerAvailable) await SendAsync(new SetRoomMixer((int)volume.Value, (int)bass.Value, (int)voice.Value, (int)treble.Value, (int)reduction.Value));
    }
    private AudioControlsChanged confirmedAudio = new(100, 100, true);
    private RoomMixerChanged confirmedMixer = new(100, 0, 0, 0);
    private async Task ShowVideoAsync()
    {
        using var dialogTranslations = translations.BeginScope();
        if (!session.Ready) throw new InvalidOperationException("Engine is not ready.");
        resolution.SelectedItem = session.Settings.VideoResolution;
        bitrate.Value = session.Settings.VideoBitrateKbps ?? 4000;
        var panel = Stack();
        Field(panel, "الدقة", "Resolution", resolution); Field(panel, "معدل الإرسال kbps", "Video bitrate kbps", bitrate);
        try
        {
            if (!await DialogAsync("ضبط الفيديو", "Video controls", panel)) return;
            if (session.Calls!.IsConnected && capabilities?.VideoControl != true) throw new InvalidOperationException("Video control has not been confirmed.");
            await session.SaveVideoPreferencesAsync((VideoResolution)resolution.SelectedItem, (int)bitrate.Value, lifetime.Token);
        }
        finally { panel.Children.Clear(); }
    }
    private async Task ShowRecordingAsync()
    {
        using var dialogTranslations = translations.BeginScope();
        var selected = sharedSource;
        _ = OpalRecordingPolicy.Start(CanStartSharedRecording, videoState, selected);
        var panel = Stack();
        var mic = new CheckBox { Content = T("تضمين الميكروفون", "Include microphone"), IsChecked = true }; var roomAudio = new CheckBox { Content = T("تضمين صوت الغرفة", "Include room audio"), IsChecked = true };
        panel.Children.Add(Text("تسجيل المصدر المشترك: " + selected!.Name, "Record shared source: " + selected!.Name));
        panel.Children.Add(Text($"جودة التسجيل: Full HD {OpalEngineCommandSink.RecordingWidth}×{OpalEngineCommandSink.RecordingHeight}",
            $"Recording quality: Full HD {OpalEngineCommandSink.RecordingWidth}×{OpalEngineCommandSink.RecordingHeight}"));
        panel.Children.Add(Text("يُسجل المحتوى المرسل للطلاب فقط.", "Only the shared content sent to students is recorded."));
        panel.Children.Add(Text("يتوقف التسجيل مؤقتاً عند توقف المشاركة أو تغيير مصدرها. شارك شاشة أو نافذة ثم اضغط استئناف.",
            "Stopping or changing sharing pauses recording. Share a display or window, then choose Resume."));
        panel.Children.Add(mic); panel.Children.Add(roomAudio);
        var chooseFolder = new Button { Content = T("اختيار مجلد التسجيلات", "Choose recording folder") };
        chooseFolder.Click += async (_, _) => { try { await ChooseRecordingFolderAsync(); } catch (Exception error) { ReportError("recording-folder", error.Message); } }; panel.Children.Add(chooseFolder);
        if (await DialogAsync("تسجيل المحاضرة", "Lecture recording", panel))
        {
            if (sharedSource?.Id != selected.Id) throw new InvalidOperationException(OpalRecordingPolicy.SharingRequired);
            await SendAsync(OpalRecordingPolicy.Start(CanStartSharedRecording, videoState, sharedSource, mic.IsChecked == true, roomAudio.IsChecked == true));
        }
    }
    private async Task ChooseRecordingFolderAsync()
    {
        var picker = new Windows.Storage.Pickers.FolderPicker(); picker.FileTypeFilter.Add("*");
        WinRT.Interop.InitializeWithWindow.Initialize(picker, WinRT.Interop.WindowNative.GetWindowHandle(this));
        var selected = await picker.PickSingleFolderAsync(); if (selected is not null) RecordingLocationStore.ForCurrentUser().Save(selected.Path);
    }
    private ToggleSwitch CreateAacToggle(CallProtocol selectedProtocol, bool enabled, bool available)
    {
        var supported = session.SupportedStudios.Allows(destination.Text, selectedProtocol, true);
        return new ToggleSwitch { Header = T("تفعيل AAC", "Enable AAC"), IsOn = enabled && supported, IsEnabled = available && supported,
            OnContent = T("AAC مع G.722 كبديل", "AAC with G.722 fallback"), OffContent = "G.722" };
    }
    private async Task ShowConnectionAsync()
    {
        using var dialogTranslations = translations.BeginScope();
        session.RequireIdle(); var current = session.Tuning; var panel = Stack();
        var availability = session.Engine!.AacAvailability;
        var aacSupported = session.SupportedStudios.Allows(destination.Text, session.Calls!.SelectedProtocol, true);
        var aac = CreateAacToggle(session.Calls.SelectedProtocol, current.AacEnabled, availability.Available);
        var aacCodec = session.Family == EngineFamily.Cisco ? AudioSelection.AacLd : AudioSelection.AacLc;
        var formats = new ComboBox();
        void UpdateFormats()
        {
            var options = session.SupportedStudios.AllowedFormats(destination.Text, session.Calls!.SelectedProtocol, session.Family, aac.IsOn);
            formats.ItemsSource = options; formats.SelectedItem = options.FirstOrDefault(x => x.Channels == current.AudioChannels && x.Kbps == current.AudioKbps) ?? options.FirstOrDefault();
        }
        UpdateFormats(); aac.Toggled += (_, _) => UpdateFormats();
        var h323 = session.Calls!.SelectedProtocol == CallProtocol.H323;
        var minimum = Number(current.JitterMinimumMs, 100, 600); var maximum = Number(current.JitterMaximumMs, 100, 600);
        var initial = Number(h323 ? current.JitterMinimumMs : current.JitterInitialMs, 100, 600);
        var buffer = Choice(new[] { 20, 40, 60, 100, 150, 200 }, current.SendBufferMs); var packet = Choice(new[] { 0, 20 }, current.PacketMs);
        minimum.IsEnabled = maximum.IsEnabled = initial.IsEnabled = buffer.IsEnabled = false;
        FrameworkElement EnvironmentField(Control field, bool initialBuffer = false)
        {
            var help = T("يُطبق إعداد VPN الخارجي تلقائيا.", "The outside VPN profile is applied automatically.");
            if (initialBuffer && h323) help += T(" يبدأ H.323 من الحد الأدنى للمخزن.", " H.323 starts at the minimum buffer.");
            // The enabled wrapper keeps the tooltip reachable around a read-only field.
            var wrapper = new Border { Child = field, Background = new Microsoft.UI.Xaml.Media.SolidColorBrush(Microsoft.UI.Colors.Transparent) };
            ToolTipService.SetToolTip(field, help); ToolTipService.SetToolTip(wrapper, help);
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetHelpText(field, help);
            return wrapper;
        }
        var videoRate = Number(current.VideoCeilingKbps, 128, 20000); var budget = Number(current.TotalSendCeilingKbps, 0, 30000); var fps = Choice(new[] { 2, 5, 10, 15, 24, 30 }, current.ScreenFps); var cameraFps = Choice(new[] { 5, 10, 15, 24, 30 }, current.CameraFps); var mode = Choice(Enum.GetValues<ScreenContentMode>(), current.ContentMode);
        panel.Children.Add(aac);
        if (!aacSupported) panel.Children.Add(Text("AAC غير معتمد لهذا الاستديو والبروتوكول؛ استخدم G.722.", "AAC is unverified for this studio and protocol; use G.722."));
        panel.Children.Add(Text(availability.Available ? "AAC جاهز. يتطلب تغييره إنهاء المكالمة." : "AAC غير متاح: " + availability.Detail,
            availability.Available ? "AAC is ready. Change it before a call." : "AAC unavailable: " + availability.Detail));
        Field(panel, "صيغة الصوت", "Audio format", formats);
        panel.Children.Add(Text("تُطبق مخازن صوت VPN الخارجية على جميع الأجهزة والبروتوكولات.",
            "Audio buffers use the outside VPN profile for all devices and protocols."));
        Field(panel, "Jitter minimum ms", "Jitter minimum ms", EnvironmentField(minimum)); Field(panel, "Jitter maximum ms", "Jitter maximum ms", EnvironmentField(maximum));
        Field(panel, "Jitter initial ms", "Jitter initial ms", EnvironmentField(initial, initialBuffer: true)); Field(panel, "Send buffer ms", "Send buffer ms", EnvironmentField(buffer)); Field(panel, "Packet ms", "Packet ms", packet);
        Field(panel, "سقف الفيديو kbps", "Video ceiling kbps", videoRate); Field(panel, "ميزانية الإرسال kbps (0 تلقائي)", "Total send budget kbps (0 automatic)", budget); Field(panel, "إطارات الشاشة / ثانية", "Screen frames / second", fps); Field(panel, "إطارات الكاميرا / ثانية", "Camera frames / second", cameraFps); Field(panel, "محتوى الشاشة", "Screen content", mode);
        if (!await DialogAsync("الصوت والشبكة", "Audio and network tuning", panel,
            dialog => dialog.IsPrimaryButtonEnabled = formats.SelectedItem is AudioFormatOption)) return;
        var format = (AudioFormatOption)formats.SelectedItem;
        await session.SaveTuningAsync(current with { AacEnabled = aac.IsOn, Mode = ConnectionPreset.Custom, AudioCodec = aac.IsOn ? aacCodec : AudioSelection.G722, AudioChannels = format.Channels, AudioKbps = format.Kbps, JitterMinimumMs = (int)minimum.Value, JitterMaximumMs = (int)maximum.Value, JitterInitialMs = (int)initial.Value, SendBufferMs = (int)buffer.SelectedItem, PacketMs = aac.IsOn ? 0 : (int)packet.SelectedItem, VideoCeilingKbps = (int)videoRate.Value, TotalSendCeilingKbps = (int)budget.Value, ScreenFps = (int)fps.SelectedItem, CameraFps = (int)cameraFps.SelectedItem, ContentMode = (ScreenContentMode)mode.SelectedItem }, lifetime.Token);
    }
    private async Task ShowDevicesAsync()
    {
        using var dialogTranslations = translations.BeginScope();
        if (!session.Ready) throw new InvalidOperationException("Engine is not ready."); using var enumerator = new MMDeviceEnumerator();
        var captures = enumerator.EnumerateAudioEndPoints(DataFlow.Capture, DeviceState.Active).ToArray(); var speakers = enumerator.EnumerateAudioEndPoints(DataFlow.Render, DeviceState.Active).ToArray();
        var mic = new ComboBox { ItemsSource = captures, DisplayMemberPath = "FriendlyName", SelectedItem = captures.FirstOrDefault(x => x.ID == session.Settings.MicrophoneId) ?? captures.FirstOrDefault() };
        var speaker = new ComboBox { ItemsSource = speakers, DisplayMemberPath = "FriendlyName", SelectedItem = speakers.FirstOrDefault(x => x.ID == session.Settings.SpeakerId) ?? speakers.FirstOrDefault() }; var name = new TextBox { Text = lecturer.Text, IsReadOnly = session.Calls?.CanHangUp == true };
        var panel = Stack(); Field(panel, "الاسم", "Display name", name); Field(panel, "الميكروفون", "Microphone", mic); Field(panel, "مخرج الصوت", "Speaker", speaker);
        panel.Children.Add(Text("تطبّق أجهزة الصوت المختارة عند إعادة الاتصال. المكالمة الحالية لا تتغير.", "Selected audio devices apply when reconnecting. The current call keeps its devices."));
        try { if (await DialogAsync("أجهزة الصوت", "Audio devices", panel)) { await session.SavePreferencesAsync(name.Text, arabic ? "ar" : "en", (mic.SelectedItem as MMDevice)?.ID, (speaker.SelectedItem as MMDevice)?.ID, lifetime.Token); lecturer.Text = name.Text; } }
        finally { foreach (var device in captures.Concat(speakers)) device.Dispose(); }
    }
    private async Task EditStudioAsync(RoomDirectoryEntry? entry)
    {
        using var dialogTranslations = translations.BeginScope();
        var panel = Stack(); var name = new TextBox { Text = entry?.Name ?? "" }; var address = new TextBox { Text = entry?.Destination ?? "", FlowDirection = FlowDirection.LeftToRight }; var profile = Choice(Enum.GetValues<EndpointProfile>(), entry?.EndpointProfile ?? EndpointProfile.Generic);
        Field(panel, "اسم الاستديو", "Studio name", name); Field(panel, "العنوان", "Address", address); Field(panel, "الجهاز", "Endpoint profile", profile);
        if (!await DialogAsync("الاستديو", "Studio", panel)) return;
        if (string.IsNullOrWhiteSpace(name.Text) || !TeleStudio.Presentation.DestinationValidator.IsValid(address.Text.Trim())) throw new ArgumentException("Enter a studio name and an IPv4 address or DNS hostname. SIP account URIs can be used in Direct call.");
        var updated = new RoomDirectoryEntry(entry?.Id ?? Guid.NewGuid().ToString("N"), name.Text.Trim(), address.Text.Trim(), (EndpointProfile)profile.SelectedItem);
        await session.SaveRoomsAsync(session.Rooms.Where(x => x.Id != updated.Id).Append(updated).ToArray(), lifetime.Token); BuildStudioCards();
    }
}
