using System.IO;
using System.Net;
using TeleStudio.Contracts;
using TeleStudio.Settings;

namespace TeleStudio.Desktop;

/// <summary>OPAL transport with the existing call, recording and surface validation boundary.</summary>
public sealed class OpalEngineCommandSink : NativeEngineCommandSink
{
    public const int RecordingWidth = 1920;
    public const int RecordingHeight = 1080;
    protected override string FormatRecordingStart(StartLectureRecording command, string encodedSourceId) =>
        base.FormatRecordingStart(command, encodedSourceId) + $" {RecordingWidth} {RecordingHeight}";
    protected override bool AcceptAutomaticRecordingPause => true;
    protected override bool AcceptQueuedRecordingBeforeFinalization => true;
    private readonly SharedOpalConsole _worker;
    private readonly EngineFamily _family;
    private readonly SemaphoreSlim _opalGate = new(1, 1);
    private CallProtocol _protocol = CallProtocol.Sip;
    private ConnectionTuning _tuning = new() { AudioCodec = AudioSelection.G722 };
    private NetworkConnectionMode _connectionMode = NetworkConnectionMode.ExternalVpn;
    private bool _tuningApplied;
    private EndpointProfile _endpoint = EndpointProfile.Auto;
    private string _audioDevices = "audio-devices default default";
    public AacAvailabilityChanged AacAvailability { get; private set; } = new(false, "Waiting for OPAL AAC plugin status.");
    public OpalStudioCatalog? StudioSupport { get; set; }
    public int? WorkerProcessId => _worker.ProcessId;
    public long WorkerProcessGeneration => _worker.ProcessGeneration;
    public EchoCancellationChanged EchoState { get; private set; } = new(false, false);

    public OpalEngineCommandSink(SharedOpalConsole worker, EngineFamily family)
        : base(worker.H323, worker.Sip, sharedWorkerLifecycle: true)
    {
        _worker = worker; _family = family;
        _tuning = OpalEditionDefaults.Tuning(family);
        ConfigureVideoStartup(new(_tuning.CameraResolution, _tuning.VideoCeilingKbps));
        SelectedSipEngine = SipEngineKind.Opal;
        EventReceived += (_, value) => { if (value is AacAvailabilityChanged aac) AacAvailability = aac; else if (value is EchoCancellationChanged echo) EchoState = echo; };
    }

    public static OpalEngineCommandSink CreateDefault(EngineFamily family, int sipPort = 5140, int h323Port = 1760, int rtpPort = 43000, string? interfaceAddress = null, string? baseDirectory = null, Func<(string? MicrophoneId, string? SpeakerId)>? audioDevices = null)
    {
        var executable = Path.Combine(ResolveOpalDirectory(baseDirectory), "opal_console.exe");
        var process = new ProtocolConsoleProcess(CallProtocol.Sip, executable, BuildOpalArguments(sipPort, h323Port, rtpPort, interfaceAddress),
            "Listening SIP on ", environment: () =>
            {
                var devices = audioDevices?.Invoke();
                var values = new Dictionary<string, string>();
                if (devices?.MicrophoneId is { Length: > 0 } microphone) values["ESSASTUDIO_MICROPHONE_ID"] = microphone;
                if (devices?.SpeakerId is { Length: > 0 } speaker) values["ESSASTUDIO_SPEAKER_ID"] = speaker;
                return values;
            }, additionalReadyPrefixes: ["Listening H.323 on "], sharedProtocols: true, shutdownTimeout: OpalCallLifecycle.TerminationTimeout);
        process.LineReceived += (_, line) =>
        {
            if (line.IsError && line.TryGetLegacyText(out var text) && text.StartsWith("startup error:", StringComparison.Ordinal))
                Runtime.DiagnosticLog.Write("opal-startup", text);
        };
        return new(new SharedOpalConsole(process), family);
    }

    public static string ResolveOpalDirectory(string? baseDirectory = null)
    {
        var path = Path.Combine(baseDirectory ?? AppContext.BaseDirectory, "Engines", "Opal", "NativeEngines");
        if (!File.Exists(Path.Combine(path, "opal_console.exe"))) throw new FileNotFoundException("The bundled OPAL worker is missing.", Path.Combine(path, "opal_console.exe"));
        return path;
    }

    internal static string BuildOpalArguments(int sipPort, int h323Port, int rtpPort, string? interfaceAddress)
    {
        if (sipPort is < 1 or > 65535 || h323Port is < 1 or > 65535 || sipPort == h323Port || rtpPort is < 1024 or > 65136 || rtpPort % 2 != 0)
            throw new ArgumentOutOfRangeException(nameof(sipPort), "OPAL requires distinct valid SIP/H.323 listener ports and an even RTP port from 1024 to 65136, reserving ports through RTP base +398 for BFCP.");
        if (interfaceAddress is not null && (!IPAddress.TryParse(interfaceAddress, out var address) || address.AddressFamily != System.Net.Sockets.AddressFamily.InterNetwork))
            throw new ArgumentException("The OPAL interface must be an IPv4 address.", nameof(interfaceAddress));
        return $"--sip-port {sipPort} --h323-port {h323Port} --rtp-port {rtpPort}" + (interfaceAddress is null ? "" : " --interface " + interfaceAddress);
    }

    public ConnectionTuning NormalizeTuning(ConnectionTuning tuning, CallProtocol protocol)
    {
        var buffers = ConnectionTuning.Preset(ConnectionPreset.External);
        tuning = tuning with { JitterMinimumMs = buffers.JitterMinimumMs, JitterMaximumMs = buffers.JitterMaximumMs,
            JitterInitialMs = buffers.JitterInitialMs, SendBufferMs = buffers.SendBufferMs };
        if (!tuning.AacEnabled) return tuning with { AudioCodec = AudioSelection.G722, AudioChannels = 1, AudioKbps = 64 };
        var codec = _family == EngineFamily.Cisco ? AudioSelection.AacLd : AudioSelection.AacLc;
        var formats = AudioFormatOptions.For(codec, protocol);
        var format = formats.FirstOrDefault(f => f.Channels == tuning.AudioChannels) ?? formats[0];
        return tuning with { AudioCodec = codec, AudioChannels = format.Channels, AudioKbps = format.Kbps, PacketMs = 0 };
    }

    public void ConfigureAudioDevices(string? microphoneId, string? speakerId)
    {
        static string Encode(string? value)
        {
            if (string.IsNullOrEmpty(value)) return "default";
            var bytes = new System.Text.UTF8Encoding(false, true).GetBytes(value);
            if (bytes.Length > 1024) throw new ArgumentException("Audio device identifier is too long.");
            return Convert.ToBase64String(bytes).TrimEnd('=').Replace('+', '-').Replace('/', '_');
        }
        Volatile.Write(ref _audioDevices, $"audio-devices {Encode(microphoneId)} {Encode(speakerId)}");
    }

    public new async Task ActivateProtocolAsync(CallProtocol protocol, CancellationToken token)
    {
        ThrowIfDisposed();
        using var commandLifetime = CancellationTokenSource.CreateLinkedTokenSource(token, RetirementToken);
        token = commandLifetime.Token;
        await WaitForCommandGateAsync(_opalGate, token);
        try { await base.ActivateProtocolAsync(protocol, token); await _worker.SelectProtocolAsync(protocol, token); _protocol = protocol; }
        finally { _opalGate.Release(); }
    }

    public override async Task SendAsync(EngineCommand command, CancellationToken token)
    {
        ThrowIfDisposed();
        using var commandLifetime = CancellationTokenSource.CreateLinkedTokenSource(token, RetirementToken);
        token = commandLifetime.Token;
        await WaitForCommandGateAsync(_opalGate, token);
        try
        {
            if (command is SetDesktopEnabled { Enabled: true } desktop)
                await ConsoleFor(_protocol).WriteLineAsync(SharingModeCommand(desktop.TransmissionMode), token);
            if (command is SetEchoCancellation echo)
            {
                await ConsoleFor(_protocol).WriteLineAsync(echo.Enabled ? "echo-cancellation on" : "echo-cancellation off", token);
                return;
            }
            if (command is SetNetworkMode network)
            {
                network = new SetNetworkMode(NetworkConnectionMode.ExternalVpn);
                var previous = _connectionMode;
                await base.SendAsync(network, token);
                _connectionMode = network.Mode;
                if (_tuningApplied)
                {
                    try { await ApplyTuningAsync(NormalizeTuning(_tuning, _protocol), token); }
                    catch
                    {
                        _connectionMode = previous;
                        await base.SendAsync(new SetNetworkMode(previous), CancellationToken.None);
                        await ApplyTuningAsync(NormalizeTuning(_tuning, _protocol), CancellationToken.None);
                        throw;
                    }
                }
                return;
            }
            if (command is SetMediaPreferences preferences)
            {
                var endpoint = preferences.Endpoint == EndpointProfile.Auto
                    ? (_family == EngineFamily.Sony ? EndpointProfile.SonyXg77 : EndpointProfile.Sx20)
                    : preferences.Endpoint;
                await base.SendAsync(preferences with { Endpoint = endpoint }, token);
                _endpoint = endpoint;
                return;
            }
            if (command is SetConnectionTuning tuning)
            {
                if (HasActiveOrPendingCall && tuning.Settings.AacEnabled != _tuning.AacEnabled) throw new InvalidOperationException("End the call before changing AAC.");
                if (tuning.Settings.AacEnabled && !AacAvailability.Available) throw new InvalidOperationException(AacAvailability.Detail);
                var normalized = NormalizeTuning(tuning.Settings, _protocol);
                normalized.Validate(_family, _protocol);
                await ApplyTuningAsync(normalized, token);
                return;
            }
            if (command is Dial dial)
            {
                if (HasActiveOrPendingCall) throw new InvalidOperationException("A call is already active or pending.");
                if (StudioSupport?.RequireAllowed(dial.Destination, dial.Protocol, _tuning.AacEnabled, _tuning.AudioChannels, _tuning.AudioKbps) is { } studio)
                {
                    if (studio.Family != _family || _endpoint != studio.EndpointProfile)
                        throw new InvalidOperationException("Select the verified device profile for this studio before calling.");
                    command = dial = dial with { Destination = studio.Address };
                }
                _protocol = dial.Protocol;
                _tuning = NormalizeTuning(_tuning, dial.Protocol);
                var effective = AacAvailability.Available ? _tuning : NormalizeTuning(_tuning with { AacEnabled = false }, dial.Protocol);
                await base.SendAsync(new SetNetworkMode(NetworkConnectionMode.ExternalVpn), token);
                await base.SendAsync(new SetConnectionTuning(effective), token);
                await ConsoleFor(dial.Protocol).WriteLineAsync(Volatile.Read(ref _audioDevices), token);
                await ConsoleFor(dial.Protocol).WriteLineAsync(effective.AacEnabled ? "aac-enabled on" : "aac-enabled off", token);
            }
            await base.SendAsync(command, token);
        }
        finally { _opalGate.Release(); }
    }

    private async Task ApplyTuningAsync(ConnectionTuning normalized, CancellationToken token)
    {
        if (!HasActiveOrPendingCall)
        {
            var endpoint = _endpoint == EndpointProfile.Auto ? (_family == EngineFamily.Sony ? EndpointProfile.SonyXg77 : EndpointProfile.Sx20) : _endpoint;
            var console = ConsoleFor(_protocol);
            await console.WriteLineAsync(EndpointCommand(endpoint, _protocol), token);
            await console.WriteLineAsync(Volatile.Read(ref _audioDevices), token);
            await console.WriteLineAsync(normalized.AacEnabled ? "aac-enabled on" : "aac-enabled off", token);
            await ConfirmIdleConnectionTuningAsync(_protocol, normalized, token);
        }
        await base.SendAsync(new SetConnectionTuning(normalized), token);
        _tuning = normalized; _tuningApplied = true;
    }

    private IProtocolConsole ConsoleFor(CallProtocol protocol) => protocol == CallProtocol.Sip ? _worker.Sip : _worker.H323;

    internal static string SharingModeCommand(SharingTransmissionMode mode) => mode switch
    {
        SharingTransmissionMode.Auto => "share-mode auto",
        SharingTransmissionMode.MainOnly => "share-mode main",
        SharingTransmissionMode.MainAndPresentation => "share-mode dual",
        _ => throw new ArgumentOutOfRangeException(nameof(mode))
    };
}
