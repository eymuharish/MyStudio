using System.IO;
using System.Text;
using System.Text.Json;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using TeleStudio.Contracts;
using TeleStudio.Desktop;
using TeleStudio.Settings;

namespace MyStudio.WinUI;

public sealed partial class MainWindow
{
    private readonly OpalStudioObservations studioObservations = new(Path.Combine(AppDataPaths.Root, "studio-observations.json"));
    private readonly Dictionary<Type, DateTimeOffset> nextSupportSample = [];
    private void LogSupportEvent(EngineEvent value)
    {
        if (value is SipConnectionHealthChanged or TransportStatistics or RoomAudioMeterChanged)
        {
            var now = DateTimeOffset.UtcNow;
            var type = value.GetType();
            if (nextSupportSample.TryGetValue(type, out var next) && now < next) return;
            nextSupportSample[type] = now.AddSeconds(10);
        }
        if (value is not (LocalSurfaceChanged or RemoteSurfaceChanged))
            WriteLog("engine/" + value.GetType().Name, JsonSerializer.Serialize(value, value.GetType()));
    }
    private async Task ShowStudioSupportAsync(RoomDirectoryEntry entry)
    {
        var body = new StackPanel { Spacing = 12 };
        body.Children.Add(new TextBlock { Text = session.SupportedStudios.Notes(entry.Destination, arabic), TextWrapping = TextWrapping.Wrap });
        var latest = studioObservations.Find(entry.Destination);
        var summary = T("لم تُسجل محاولة اتصال حديثة في هذه النسخة.", "No recent call attempt is recorded in this copy.");
        if (latest is not null)
        {
            var state = latest.State switch
            {
                "dialing" => T("محاولة اتصال", "Dial attempt"), "connected" => T("اتصال قائم", "Connected"),
                "ended" => T("انتهت المكالمة", "Call ended"), _ => T("فشل الاتصال", "Call failed")
            };
            summary = $"{latest.UpdatedAt.ToLocalTime():yyyy-MM-dd HH:mm:ss zzz} · {state} · {latest.Protocol}";
            if (latest.AudioCodec is { } codec) summary += "\n" + T("الترميز المبلغ عنه", "Reported codec") + ": " + codec;
            if (latest.DecodedAudioSamples is { } audio) summary += "\n" + T("عينات الصوت المفكوكة", "Decoded audio samples") + ": " + audio;
            if (latest.DeviceSubmittedNonzeroSamples is { } played) summary += "\n" + T("عينات غير صامتة أرسلت لجهاز الصوت", "Nonzero samples submitted to audio device") + ": " + played;
            if (latest.DecodedVideoFrames is { } video) summary += "\n" + T("إطارات الفيديو المستلمة", "Received video frames") + ": " + video;
            if (latest.FailureCode is { } error) summary += "\n" + T("رمز الخطأ", "Error code") + ": " + error;
        }
        body.Children.Add(new TextBlock { Text = T("آخر حالة اتصال", "Latest call status"), FontSize = 18 });
        body.Children.Add(new TextBlock { Text = summary, TextWrapping = TextWrapping.Wrap });
        body.Children.Add(new TextBlock { Text = T("تتحدث الحالة من أحداث المكالمات. اعتماد بروتوكول أو ترميز جديد يحتاج اختبارًا موثقًا؛ لا تثبت العدادات وحدها جودة الصوت أو العرض على الجهاز الآخر.", "Status updates from call events. Approving a new protocol or codec requires a documented test; counters alone do not prove audible audio or the other device's display."), TextWrapping = TextWrapping.Wrap });
        await new ContentDialog { Title = entry.Name + " · " + T("ملاحظات الدعم", "Support notes"), Content = new ScrollViewer { Content = body, MaxHeight = 520 }, CloseButtonText = T("إغلاق", "Close"), XamlRoot = navigation.XamlRoot }.ShowAsync();
    }
    private async Task ExportDiagnosticsAsync()
    {
        RefreshDiagnostics();
        var folder = Path.Combine(AppDataPaths.Root, "Diagnostics"); Directory.CreateDirectory(folder);
        var path = Path.Combine(folder, "MyStudio-log-" + DateTime.Now.ToString("yyyyMMdd-HHmmss-fff") + ".txt");
        var buildPath = Path.Combine(AppContext.BaseDirectory, "BUILD-IDENTITY.json");
        var snapshot = new
        {
            session.Ready, session.Error, session.Family, session.ConnectionMode, Destination = session.TargetDestination,
            Protocol = session.Calls?.SelectedProtocol, Connected = session.Calls?.IsConnected,
            Endpoint = session.Settings.Endpoint, session.Settings.VideoResolution, session.Settings.VideoBitrateKbps,
            session.Settings.MicrophoneId, session.Settings.SpeakerId, session.Tuning,
            VideoState = videoState, SelectedSharingSource = sharedSource, Recording = recording,
            MicrophoneMuted = micMuted, RoomMuted = roomMuted, SystemAudio = systemEnabled,
            PresentedRemoteFrames = remotePresenter.PresentedFrames, PresentedLocalFrames = localPresenter.PresentedFrames,
            LiveDiagnostics = diagnostics.Text, SupportedStudios = session.SupportedStudios,
            EchoCancellation = session.Engine?.EchoState
        };
        var report = OpalSupportReport.Build(snapshot, eventEvidence.OfType<EngineEvent>(), studioObservations.Snapshot(),
            ReadMyStudioLog(full: true), File.Exists(buildPath) ? File.ReadAllText(buildPath) : "Build identity unavailable");
        File.WriteAllText(path, report, new UTF8Encoding(true));
        var result = await new ContentDialog
        {
            Title = T("تم حفظ سجل الدعم", "Support log saved"),
            Content = new TextBlock { Text = path, TextWrapping = TextWrapping.Wrap, IsTextSelectionEnabled = true },
            PrimaryButtonText = T("فتح المجلد", "Open folder"), CloseButtonText = T("إغلاق", "Close"), XamlRoot = navigation.XamlRoot
        }.ShowAsync();
        if (result == ContentDialogResult.Primary) await Windows.System.Launcher.LaunchFolderAsync(await Windows.Storage.StorageFolder.GetFolderFromPathAsync(folder));
    }
    private async Task RemoveStudioAsync(RoomDirectoryEntry entry)
    {
        session.RequireIdle();
        var result = await new ContentDialog
        {
            Title = T("حذف الاستديو من القائمة؟", "Remove studio from the list?"),
            Content = new TextBlock { Text = entry.Name + " · " + entry.Destination, TextWrapping = TextWrapping.Wrap },
            PrimaryButtonText = T("حذف", "Remove"), CloseButtonText = T("إلغاء", "Cancel"),
            DefaultButton = ContentDialogButton.Close, XamlRoot = navigation.XamlRoot
        }.ShowAsync();
        if (result != ContentDialogResult.Primary) return;
        await session.SaveRoomsAsync(session.Rooms.Where(item => item.Id != entry.Id).ToArray(), lifetime.Token);
        BuildStudioCards();
    }
}
