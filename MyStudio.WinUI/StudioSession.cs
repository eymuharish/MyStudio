using System.IO;
using TeleStudio.Contracts;
using TeleStudio.Desktop;
using TeleStudio.Presentation;
using TeleStudio.Settings;

namespace MyStudio.WinUI;

/// <summary>WinUI composition over the existing tested console, settings and presentation boundary.</summary>
public sealed class StudioSession : IAsyncDisposable
{
    public EngineFamily Family { get; private set; } = EngineFamily.Cisco;
    public OpalEngineCommandSink? Engine { get; private set; }
    public MainViewModel? Calls { get; private set; }
    public AppSettings Settings { get; private set; } = AppSettings.Default;
    public ConnectionTuning Tuning { get; private set; } = new() { AudioCodec = AudioSelection.G722 };
    public NetworkConnectionMode ConnectionMode => connectionEnvironment.Mode;
    public OpalStudioCatalog SupportedStudios { get; } = LoadSupportedStudios();
    public string TargetDestination { get; set; } = "";
    public IReadOnlyList<RoomDirectoryEntry> Rooms { get; private set; } = [];
    public NativeCaptureCatalog? Catalog { get; private set; }
    public NativeCaptureCatalog? Cameras { get; private set; }
    public bool Ready { get; private set; }
    internal int InitializationCount { get; private set; }
    public string? Error { get; private set; }
    public event EventHandler<EngineEvent>? EventReceived;
    private SettingsStore? settingsStore;
    private readonly ConnectionTuningStore tuningStore = new(Path.Combine(AppDataPaths.Root, "profiles"));
    private readonly RoomDirectoryStore roomStore = new();
    private readonly OpalConnectionEnvironment connectionEnvironment = new(new OpalConnectionModeStore(Path.Combine(AppDataPaths.Root, "connection-environment.json")));
    private static OpalStudioCatalog LoadSupportedStudios()
    {
        using var stream = typeof(StudioSession).Assembly.GetManifestResourceStream("MyStudio.WinUI.OpalSupportedStudios")
            ?? throw new InvalidOperationException("The studio support catalog is missing.");
        using var reader = new StreamReader(stream);
        return OpalStudioCatalog.Parse(reader.ReadToEnd());
    }

    public async Task InitializeAsync(EngineFamily family, CancellationToken token)
    {
        if (Calls?.CanHangUp == true) throw new InvalidOperationException("End the call before changing device family.");
        InitializationCount++;
        Ready = false;
        if (Engine is { } old) { await old.DisposeAsync(); Engine = null; }
        if (Catalog is not null) await Catalog.DisposeAsync();
        if (Cameras is not null) await Cameras.DisposeAsync();
        Family = family; Error = null;
        var settingsPath = Path.Combine(AppDataPaths.Root, "profiles", family.ToString(), "settings.json");
        settingsStore = new SettingsStore(settingsPath);
        Settings = File.Exists(settingsPath) ? await settingsStore.LoadAsync(token) : OpalEditionDefaults.Settings(family);
        Settings = OpalEditionDefaults.UpgradeVideoSettings(Settings);
        await connectionEnvironment.LoadAsync(token);
        var arguments = Environment.GetCommandLineArgs(); var backendOption = Array.IndexOf(arguments, "--sip-engine");
        if (backendOption >= 0 && (backendOption + 1 >= arguments.Length || !string.Equals(arguments[backendOption + 1], "opal", StringComparison.OrdinalIgnoreCase)))
            throw new ArgumentException("This independent build uses OPAL. --sip-engine accepts Opal only.");
        Settings = AppSettingsCopy.Copy(Settings, Settings.LecturerName, Settings.Language, networkMode: ConnectionMode, sipEngine: SipEngineKind.Opal);
        await settingsStore.SaveAsync(Settings, token);
        Rooms = SupportedStudios.ApplyDirectory(await roomStore.LoadAsync(token), addMissing: !File.Exists(Path.Combine(AppDataPaths.Root, "rooms.json")));
        try
        {
            static int PortOption(string[] args, string name, int fallback)
            {
                var option = Array.IndexOf(args, name); if (option < 0) return fallback;
                if (option + 1 >= args.Length || !int.TryParse(args[option + 1], out var port) || port is < 1 or > 65535) throw new ArgumentException(name + " requires a port from 1 to 65535.");
                return port;
            }
            var interfaceOption = Array.IndexOf(arguments, "--interface");
            if (interfaceOption >= 0 && interfaceOption + 1 >= arguments.Length) throw new ArgumentException("--interface requires an IPv4 address.");
            Engine = OpalEngineCommandSink.CreateDefault(family, PortOption(arguments, "--sip-port", 5140), PortOption(arguments, "--h323-port", 1760), PortOption(arguments, "--rtp-port", 43000),
                interfaceOption < 0 ? null : arguments[interfaceOption + 1], audioDevices: () => (Settings.MicrophoneId, Settings.SpeakerId));
            var active = Engine;
            active.StudioSupport = SupportedStudios;
            active.ConfigureAudioDevices(Settings.MicrophoneId, Settings.SpeakerId);
            active.EventReceived += (_, value) => { if (ReferenceEquals(active, Engine)) EventReceived?.Invoke(this, value); };
            Calls = new MainViewModel(active);
            Calls.TryAcceptLecturerName(Settings.LecturerName == "EssaStudio" ? "MyStudio" : Settings.LecturerName, out _);
            Calls.SelectedEndpoint = Settings.Endpoint;
            Calls.SelectedNetworkMode = ConnectionMode;
            Calls.SelectedCodec = CodecPreference.H264;
            Calls.SetLanguage(Settings.Language == "en" ? AppLanguage.English : AppLanguage.Arabic);
            active.ConfigureVideoStartup(new(Settings.VideoResolution, Settings.VideoBitrateKbps));
            await active.StartAsync(token);
            await active.ActivateProtocolAsync(CallProtocol.Sip, token);
            await connectionEnvironment.ApplyAsync(active, Calls, token);
            await active.SendAsync(new SetMediaPreferences(CodecPreference.H264, Calls.SelectedQuality, Calls.SelectedEndpoint), token);
            Tuning = await LoadTuningAsync(CallProtocol.Sip, token);
            Settings = AppSettingsCopy.CopyVideo(Settings, Tuning.CameraResolution, Tuning.VideoCeilingKbps, 1);
            await settingsStore.SaveAsync(Settings, token);
            await active.SendAsync(new SetVideoPreferences(Tuning.CameraResolution, Tuning.VideoCeilingKbps), token);
            if (Tuning.AacEnabled && (!active.AacAvailability.Available || !SupportedStudios.Allows(TargetDestination, CallProtocol.Sip, true)))
                Tuning = active.NormalizeTuning(Tuning with { AacEnabled = false }, CallProtocol.Sip);
            Tuning = SupportedTuning(Tuning, CallProtocol.Sip);
            await active.SendAsync(new SetConnectionTuning(Tuning), token);
            var native = OpalEngineCommandSink.ResolveOpalDirectory();
            Catalog = new NativeCaptureCatalog(native); Cameras = new NativeCaptureCatalog(native, cameras: true);
            Ready = true;
        }
        catch (Exception error) { Error = error.Message; Ready = false; throw; }
    }
    public async Task SelectProtocolAsync(CallProtocol protocol, CancellationToken token)
    {
        RequireIdle();
        if (!SupportedStudios.AllowedProtocols(TargetDestination).Contains(protocol))
            throw new InvalidOperationException("This protocol has no verified working connection for the selected studio.");
        await Engine!.ActivateProtocolAsync(protocol, token);
        Calls!.SelectedProtocol = protocol;
        await connectionEnvironment.ApplyAsync(Engine, Calls, token);
        Tuning = await LoadTuningAsync(protocol, token);
        await Engine.SendAsync(new SetVideoPreferences(Tuning.CameraResolution, Tuning.VideoCeilingKbps), token);
        Settings = AppSettingsCopy.CopyVideo(Settings, Tuning.CameraResolution, Tuning.VideoCeilingKbps, 1);
        await settingsStore!.SaveAsync(Settings, token);
        if (Tuning.AacEnabled && (!Engine.AacAvailability.Available || !SupportedStudios.Allows(TargetDestination, protocol, true)))
            Tuning = Engine.NormalizeTuning(Tuning with { AacEnabled = false }, protocol);
        Tuning = SupportedTuning(Tuning, protocol);
        await Engine.SendAsync(new SetConnectionTuning(Tuning), token);
    }
    public async Task SetConnectionModeAsync(NetworkConnectionMode mode, CancellationToken token)
    {
        RequireIdle();
        await connectionEnvironment.ChangeAsync(mode, Ready, Engine!, Calls!, token);
        Tuning = Engine!.NormalizeTuning(Tuning, Calls!.SelectedProtocol);
        Settings = AppSettingsCopy.Copy(Settings, Settings.LecturerName, Settings.Language, networkMode: ConnectionMode);
    }
    public async Task PrepareStudioAsync(OpalSupportedStudio studio, bool recommend, CancellationToken token)
    {
        RequireIdle();
        if (Family != studio.Family) throw new InvalidOperationException("Select the studio device family first.");
        Calls!.SelectedEndpoint = studio.EndpointProfile;
        await Engine!.SendAsync(new SetMediaPreferences(CodecPreference.H264, Calls.SelectedQuality, studio.EndpointProfile), token);
        var recommendation = SupportedStudios.Recommendation(studio);
        if (recommendation is null)
        {
            Tuning = Engine.NormalizeTuning(Tuning with { AacEnabled = false }, Calls.SelectedProtocol);
            await Engine.SendAsync(new SetConnectionTuning(Tuning), token);
            return;
        }
        var selected = recommend || !SupportedStudios.AllowedProtocols(TargetDestination).Contains(Calls.SelectedProtocol)
            ? recommendation.Value.Protocol : Calls.SelectedProtocol;
        if (selected != Calls.SelectedProtocol) await SelectProtocolAsync(selected, token);
        var aac = recommend ? recommendation.Value.AacEnabled : Tuning.AacEnabled;
        if (!Engine.AacAvailability.Available || !SupportedStudios.Allows(TargetDestination, selected, true)) aac = false;
        Tuning = SupportedTuning(Engine.NormalizeTuning(Tuning with { AacEnabled = aac }, selected), selected);
        await Engine.SendAsync(new SetConnectionTuning(Tuning), token);
    }
    public async Task SelectBackendAsync(SipEngineKind backend, CancellationToken token)
    {
        if (backend != SipEngineKind.Opal) throw new NotSupportedException("This independent build uses OPAL.");
        await Task.CompletedTask;
    }
    public async Task SaveTuningAsync(ConnectionTuning tuning, CancellationToken token)
    {
        RequireIdle(); tuning = Engine!.NormalizeTuning(tuning with { OpalAudioDefaultsVersion = 1, OpalVideoDefaultsVersion = 1 }, Calls!.SelectedProtocol);
        if (SupportedStudios.Find(TargetDestination) is not null)
            SupportedStudios.RequireAllowed(TargetDestination, Calls.SelectedProtocol, tuning.AacEnabled, tuning.AudioChannels, tuning.AudioKbps);
        tuning.Validate(Family, Calls.SelectedProtocol);
        await Engine.SendAsync(new SetConnectionTuning(tuning), token);
        await tuningStore.SaveAsync(Family, Calls.SelectedProtocol, tuning, token); Tuning = tuning;
        Settings = AppSettingsCopy.CopyVideo(Settings, tuning.CameraResolution, tuning.VideoCeilingKbps, 1);
        await settingsStore!.SaveAsync(Settings, token);
    }
    public async Task SaveVideoPreferencesAsync(VideoResolution resolution, int bitrate, CancellationToken token)
    {
        if (!Ready || Engine is null || Calls is null) throw new InvalidOperationException("Engine is not ready.");
        var tuning = Tuning with { CameraResolution = resolution, ScreenResolution = resolution,
            VideoCeilingKbps = bitrate, OpalVideoDefaultsVersion = 1 };
        tuning.Validate(Family, Calls.SelectedProtocol);
        await Engine.SendAsync(new SetConnectionTuning(tuning), token);
        await Engine.SendAsync(new SetVideoPreferences(resolution, bitrate), token);
        await tuningStore.SaveAsync(Family, Calls.SelectedProtocol, tuning, token); Tuning = tuning;
        Settings = AppSettingsCopy.CopyVideo(Settings, resolution, bitrate, 1);
        await settingsStore!.SaveAsync(Settings, token);
    }
    private async Task<ConnectionTuning> LoadTuningAsync(CallProtocol protocol, CancellationToken token)
    {
        var stored = await tuningStore.LoadAsync(Family, protocol, token, missingDefaults: OpalEditionDefaults.Tuning(Family));
        var updated = Engine!.NormalizeTuning(OpalEditionDefaults.UpgradeVideoDefaults(OpalEditionDefaults.UpgradeAudioDefaults(stored, Family)), protocol);
        if (stored.OpalAudioDefaultsVersion < 1 || stored.OpalVideoDefaultsVersion < 1) await tuningStore.SaveAsync(Family, protocol, updated, token);
        return updated;
    }
    public async Task SavePreferencesAsync(string lecturer, string language, string? microphone, string? speaker, CancellationToken token)
    {
        Settings = AppSettingsCopy.Copy(Settings, lecturer, language, microphoneId: microphone, speakerId: speaker);
        await settingsStore!.SaveAsync(Settings, token);
        Calls!.TryAcceptLecturerName(lecturer, out _);
        Engine!.ConfigureAudioDevices(microphone, speaker);
    }
    public async Task RememberDestinationAsync(string destination, CancellationToken token)
    {
        if (!RecentDestinationPolicy.IsSafe(destination)) return;
        Settings = Settings.WithRecentDestination(destination);
        await settingsStore!.SaveAsync(Settings, token);
    }
    public async Task SaveRoomsAsync(IReadOnlyList<RoomDirectoryEntry> rooms, CancellationToken token)
    { await roomStore.SaveAsync(rooms, token); Rooms = SupportedStudios.ApplyDirectory(rooms, addMissing: false); }
    public ConnectionTuning SupportedTuning(ConnectionTuning tuning, CallProtocol protocol)
    {
        var formats = SupportedStudios.AllowedFormats(TargetDestination, protocol, Family, tuning.AacEnabled);
        if (formats.Length == 0) return tuning;
        var format = formats.FirstOrDefault(x => x.Channels == tuning.AudioChannels && x.Kbps == tuning.AudioKbps) ?? formats[0];
        return tuning with { AudioChannels = format.Channels, AudioKbps = format.Kbps };
    }
    public void RequireIdle()
    {
        if (!Ready || Engine is null) throw new InvalidOperationException(Error ?? "Engine is not ready.");
        if (Calls?.CanHangUp == true) throw new InvalidOperationException("End the call before changing connection settings.");
    }
    public async ValueTask DisposeAsync()
    {
        Ready = false;
        if (Engine is { } engine) { await engine.DisposeAsync(); Engine = null; }
        if (Catalog is not null) await Catalog.DisposeAsync();
        if (Cameras is not null) await Cameras.DisposeAsync();
    }
}
