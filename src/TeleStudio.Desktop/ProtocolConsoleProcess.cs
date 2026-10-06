using System.Buffers;
using System.Diagnostics;
using System.IO;
using System.Security.Cryptography;
using TeleStudio.Contracts;

namespace TeleStudio.Desktop;

public sealed class ProtocolConsoleProcess : IProtocolConsole
{
    private const int MaximumLineBytes = 64 * 1024;
    private readonly string _executablePath;
    private readonly string _arguments;
    private readonly string _readyPrefix;
    private readonly IReadOnlyList<string> _additionalReadyPrefixes;
    private readonly bool _sharedProtocols;
    private readonly object _readyLock = new();
    private HashSet<string> _waitingReady = [];
    private readonly Func<IReadOnlyDictionary<string, string>>? _environment;
    private TaskCompletionSource? _startupReady;
    private readonly SemaphoreSlim _writeLock = new(1, 1);
    private readonly SemaphoreSlim _startLock = new(1, 1);
    private readonly CancellationTokenSource _lifetime = new();
    private readonly CancellationTokenSource _commands = new();
    private readonly object _disposeLock = new();
    private Task? _disposeTask;
    internal static readonly TimeSpan CommandTimeout = TimeSpan.FromSeconds(8);
    private Process? _process;
    private long _processGeneration;
    private bool _disposed;
    private Task _outputPump = Task.CompletedTask;
    private Task _errorPump = Task.CompletedTask;
    private int _callAwaitingTerminal;
    private SetVideoPreferences _videoStartup = new(VideoResolution.Auto, null);
    private readonly TimeSpan _shutdownTimeout;

    public ProtocolConsoleProcess(CallProtocol protocol, string executablePath, string arguments, string readyPrefix, Func<IReadOnlyDictionary<string, string>>? environment = null, IReadOnlyList<string>? additionalReadyPrefixes = null, bool sharedProtocols = false, TimeSpan? shutdownTimeout = null)
    {
        Protocol = protocol;
        _executablePath = executablePath;
        _arguments = arguments;
        _environment = environment;
        _additionalReadyPrefixes = additionalReadyPrefixes ?? [];
        _sharedProtocols = sharedProtocols;
        _shutdownTimeout = shutdownTimeout ?? TimeSpan.FromSeconds(4);
        if (_shutdownTimeout <= TimeSpan.Zero || _shutdownTimeout > TimeSpan.FromMinutes(1)) throw new ArgumentOutOfRangeException(nameof(shutdownTimeout));
        // SIP announces its listener on stderr only after native initialization succeeds.
        _readyPrefix = protocol == CallProtocol.Sip ? readyPrefix : string.Empty;
    }

    public CallProtocol Protocol { get; }
    public long ProcessGeneration => Interlocked.Read(ref _processGeneration);
    public int? ProcessId { get { try { return _process is { HasExited: false } process ? process.Id : null; } catch (InvalidOperationException) { return null; } } }
    public event EventHandler<ProtocolConsoleLine>? LineReceived;
    public event EventHandler<ProtocolConsoleGenerationEnded>? GenerationEnded;

    public void ConfigureVideoStartup(SetVideoPreferences preferences)
    {
        ArgumentNullException.ThrowIfNull(preferences);
        Volatile.Write(ref _videoStartup, preferences);
    }

    public async Task StartAsync(CancellationToken cancellationToken)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        using var commandLifetime = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken, _commands.Token);
        cancellationToken = commandLifetime.Token;
        if (!await _startLock.WaitAsync(TimeSpan.FromSeconds(20), cancellationToken))
            throw new TimeoutException("Native startup gate did not become available within its deadline.");
        try
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            if (_process is { HasExited: false })
            {
                return;
            }
            _process?.Dispose();
            _process = null;
            if (!File.Exists(_executablePath))
            {
                throw new FileNotFoundException($"Native {Protocol} engine was not found.", _executablePath);
            }

            var generation = checked(ProcessGeneration + 1);
            _startupReady = new(TaskCreationOptions.RunContinuationsAsynchronously);
            lock (_readyLock) _waitingReady = new(new[] { _readyPrefix }.Concat(_additionalReadyPrefixes).Where(prefix => !string.IsNullOrEmpty(prefix)), StringComparer.Ordinal);
            var startInfo = CreateStartInfo(_executablePath, _arguments, Volatile.Read(ref _videoStartup));
            if (_environment is not null) ApplyWorkerEnvironment(startInfo, _environment());
            var process = new Process { StartInfo = startInfo, EnableRaisingEvents = true };
            process.Exited += (_, _) => GenerationEnded?.Invoke(this, new ProtocolConsoleGenerationEnded(generation));
            if (!process.Start())
            {
                process.Dispose();
                throw new InvalidOperationException($"Native {Protocol} engine could not be started.");
            }
            try { Runtime.OwnedEngineProcesses.Register(process); }
            catch
            {
                try { if (!process.HasExited) process.Kill(entireProcessTree: true); } catch (InvalidOperationException) { }
                process.Dispose();
                throw;
            }
            var processId = process.Id;
            process.Exited += (_, _) => Runtime.OwnedEngineProcesses.Unregister(processId);
            Interlocked.Exchange(ref _processGeneration, generation);
            _process = process;
            _outputPump = ReadLinesAsync(process.StandardOutput.BaseStream, false, generation, _lifetime.Token);
            _errorPump = ReadLinesAsync(process.StandardError.BaseStream, true, generation, _lifetime.Token);

            try
            {
                if (string.IsNullOrEmpty(_readyPrefix) && _additionalReadyPrefixes.Count == 0)
                    await Task.Delay(TimeSpan.FromMilliseconds(50), cancellationToken);
                else
                {
                    using var startup = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
                    startup.CancelAfter(TimeSpan.FromSeconds(20));
                    try
                    {
                        var exited = process.WaitForExitAsync(startup.Token);
                        var completed = await Task.WhenAny(_startupReady.Task, exited).WaitAsync(startup.Token);
                        await completed; // Observe cancellation even when the exit wait wins the timeout race.
                        startup.Cancel();
                    }
                    catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
                    {
                        throw new TimeoutException($"Native {Protocol} listener did not become ready. Engine: {_executablePath}");
                    }
                }
                cancellationToken.ThrowIfCancellationRequested();
                if (process.HasExited) throw EngineExited(process);
            }
            catch
            {
                if (!process.HasExited) process.Kill(entireProcessTree: true);
                await process.WaitForExitAsync();
                await Task.WhenAll(_outputPump, _errorPump);
                Runtime.OwnedEngineProcesses.Unregister(processId);
                _process = null;
                process.Dispose();
                throw;
            }
        }
        finally
        {
            _startLock.Release();
        }
    }

    public async Task WriteLineAsync(string command, CancellationToken cancellationToken)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        ArgumentException.ThrowIfNullOrWhiteSpace(command);
        var process = _process ?? throw new InvalidOperationException($"Native {Protocol} engine has not started.");
        if (process.HasExited)
        {
            throw EngineExited(process);
        }

        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken, _commands.Token);
        deadline.CancelAfter(CommandTimeout);
        try
        {
            await _writeLock.WaitAsync(deadline.Token);
            if (_disposed) { _writeLock.Release(); throw new ObjectDisposedException(nameof(ProtocolConsoleProcess)); }
            if (command.StartsWith("call ", StringComparison.Ordinal)) Interlocked.Exchange(ref _callAwaitingTerminal, 1);
            await WriteOwnedAsync(process, command, deadline.Token);
        }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested && !_commands.IsCancellationRequested)
        { throw new TimeoutException("Native command pipe did not accept the command within its deadline."); }
    }

    // Cancellation bounds the caller even when an OS pipe write ignores it. The
    // actual writer retains the gate until it exits, so a second writer cannot race.
    private async Task WriteOwnedAsync(Process process, string command, CancellationToken token)
    {
        Task? write = null;
        try
        {
            write = WritePipeAsync(process, command, token);
            await write.WaitAsync(token);
        }
        finally
        {
            if (write is null || write.IsCompleted) _writeLock.Release();
            else _ = ReleaseWriterWhenFinishedAsync(write);
        }
    }

    private static async Task WritePipeAsync(Process process, string command, CancellationToken token)
    {
        await process.StandardInput.WriteLineAsync(command.AsMemory(), token);
        await process.StandardInput.FlushAsync(token);
    }

    private async Task ReleaseWriterWhenFinishedAsync(Task write)
    {
        try { await write; } catch (Exception) { }
        finally { _writeLock.Release(); }
    }

    private InvalidOperationException EngineExited(Process process) => new(
        $"Native {Protocol} engine stopped. Engine: {_executablePath}; exit code: {process.ExitCode} (0x{unchecked((uint)process.ExitCode):X8}). " +
        "Verify the bundled runtime files and Windows media components; see the native engine log for initialization errors.");

    public ValueTask DisposeAsync()
    {
        lock (_disposeLock)
        {
            if (_disposeTask?.IsCompleted == true && _process is null) return ValueTask.CompletedTask;
            if (_disposeTask?.IsFaulted == true) _disposeTask = null;
            return new(_disposeTask ??= DisposeCoreAsync());
        }
    }

    private async Task DisposeCoreAsync()
    {
        if (_disposed && _process is null)
        {
            return;
        }
        _disposed = true;
        _commands.Cancel();
        Exception? shutdownFailure = null;
        // Retire a pending startup before touching its process or stream ownership.
        if (!await _startLock.WaitAsync(_shutdownTimeout))
            throw new TimeoutException("Native startup did not retire; replacement was stopped.");
        _startLock.Release();

        if (_process is { HasExited: false } process)
        {
            try
            {
                // Native hangup/media cleanup logs synchronously. Stopping either pump
                // here can fill its pipe and block the command loop before it sends BYE.
                using var timeout = new CancellationTokenSource(_shutdownTimeout);
                await _writeLock.WaitAsync(timeout.Token);
                await WriteOwnedAsync(process, "quit", timeout.Token);
                await process.WaitForExitAsync(timeout.Token);
            }
            catch (Exception error) when (error is OperationCanceledException or InvalidOperationException or IOException)
            {
                shutdownFailure = error;
                try
                {
                    if (!process.HasExited) process.Kill(entireProcessTree: true);
                }
                catch (InvalidOperationException) { }
            }
        }

        if (_process is { HasExited: false } pendingExit)
        {
            try { await pendingExit.WaitForExitAsync().WaitAsync(TimeSpan.FromMilliseconds(500)); }
            catch (TimeoutException) { throw new InvalidOperationException("Native process has not released its ports; replacement was stopped."); }
        }
        // Process exit may precede the last buffered DISCONNECTED line. Drain it before
        // judging the shutdown, independently of wrapper subscriptions already retired.
        try { await Task.WhenAll(_outputPump, _errorPump).WaitAsync(TimeSpan.FromMilliseconds(200)); }
        catch (TimeoutException) { shutdownFailure ??= new IOException("Native output did not finish draining."); }
        _lifetime.Cancel();
        if (_process is { } completed)
        {
            if (completed.ExitCode != 0) shutdownFailure ??= new IOException($"Native process exited with code {completed.ExitCode}.");
            Runtime.OwnedEngineProcesses.Unregister(completed.Id);
        }
        var callUnconfirmed = Volatile.Read(ref _callAwaitingTerminal) != 0;
        if (!await _writeLock.WaitAsync(TimeSpan.FromSeconds(1)))
            throw new IOException("Native command writer has not retired; its resources remain owned and replacement was stopped.");
        try { _process?.Dispose(); _process = null; }
        finally { _writeLock.Release(); }
        // Queued callers can still observe cancellation and release a gate. These
        // managed semaphores/CTS have no allocated wait handles and are reclaimed by GC.
        if (shutdownFailure is not null || callUnconfirmed)
            throw new IOException("Native shutdown is unconfirmed; local resources were stopped, but remote call termination was not confirmed.", shutdownFailure);
    }

    internal static ProcessStartInfo CreateStartInfo(string executablePath, string arguments, SetVideoPreferences? preferences = null)
    {
        var token = RandomNumberGenerator.GetBytes(32);
        var encodedToken = Convert.ToBase64String(token).TrimEnd('=').Replace('+', '-').Replace('/', '_');
        var startInfo = new ProcessStartInfo
        {
            FileName = executablePath,
            Arguments = arguments,
            WorkingDirectory = Path.GetDirectoryName(executablePath)!,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardInput = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true
        };
        var parentProcessId = Environment.ProcessId.ToString(System.Globalization.CultureInfo.InvariantCulture);
        startInfo.Environment["ESSASTUDIO_SURFACE_HOST_PID"] = parentProcessId;
        startInfo.Environment["ESSASTUDIO_SYSTEM_AUDIO_EXCLUDE_PID"] = parentProcessId;
        startInfo.Environment["ESSASTUDIO_SURFACE_SESSION_TOKEN"] = encodedToken;
        preferences ??= new(VideoResolution.Auto, null);
        startInfo.Environment["ESSASTUDIO_VIDEO_RESOLUTION"] = preferences.Resolution switch
        {
            VideoResolution.Sd => "SD", VideoResolution.Hd => "HD", VideoResolution.Fhd => "FHD", _ => "auto"
        };
        startInfo.Environment["ESSASTUDIO_VIDEO_BITRATE_KBPS"] = preferences.BitrateKbps?.ToString(System.Globalization.CultureInfo.InvariantCulture) ?? "auto";
        var recordingDirectory = TeleStudio.Settings.RecordingLocationStore.ForCurrentUser().Load();
        if (recordingDirectory is null && TeleStudio.Settings.AppDataPaths.OverrideRoot is { } isolated)
        {
            recordingDirectory = Path.Combine(isolated, "Recordings");
            Directory.CreateDirectory(recordingDirectory);
        }
        if (recordingDirectory is not null) startInfo.Environment["ESSASTUDIO_RECORDING_DIRECTORY"] = recordingDirectory;
        return startInfo;
    }
    internal static void ApplyWorkerEnvironment(ProcessStartInfo startInfo, IReadOnlyDictionary<string, string> values)
    {
        // Clear inherited account values, then inject only this family's process-scoped configuration.
        foreach (var name in new[] { "TRANSPORT", "SRTP", "IDENTITY", "REGISTRAR", "USERNAME", "PASSWORD", "OUTBOUND_PROXY" })
            startInfo.Environment.Remove("ESSASTUDIO_SIP_" + name);
        startInfo.Environment.Remove("ESSASTUDIO_MICROPHONE_ID");
        startInfo.Environment.Remove("ESSASTUDIO_SPEAKER_ID");
        foreach (var value in values) startInfo.Environment[value.Key] = value.Value;
    }

    private async Task ReadLinesAsync(Stream stream, bool isError, long generation, CancellationToken cancellationToken)
    {
        var readBuffer = ArrayPool<byte>.Shared.Rent(4096);
        using var line = new MemoryStream();
        var dropping = false;
        try
        {
            while (true)
            {
                var count = await stream.ReadAsync(readBuffer.AsMemory(), cancellationToken);
                if (count == 0)
                {
                    if (!dropping && line.Length != 0)
                    {
                        Publish(line, isError, generation);
                    }
                    return;
                }
                var start = 0;
                for (var index = 0; index < count; index++)
                {
                    if (readBuffer[index] != (byte)'\n')
                    {
                        continue;
                    }
                    if (!dropping)
                    {
                        line.Write(readBuffer, start, index - start + 1);
                        Publish(line, isError, generation);
                    }
                    line.SetLength(0);
                    dropping = false;
                    start = index + 1;
                }
                if (start < count && !dropping)
                {
                    if (line.Length + count - start > MaximumLineBytes)
                    {
                        line.SetLength(0);
                        dropping = true;
                    }
                    else
                    {
                        line.Write(readBuffer, start, count - start);
                    }
                }
            }
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
        }
        catch (IOException)
        {
        }
        finally
        {
            ArrayPool<byte>.Shared.Return(readBuffer);
        }
    }

    private void Publish(MemoryStream line, bool isError, long generation)
    {
        if (generation == ProcessGeneration)
        {
            var message = new ProtocolConsoleLine(NormalizeStandardStreamLine(line.ToArray()), isError, generation);
            if (message.TryGetLegacyText(out var text) &&
                (ProtocolConsoleEventParser.TryParse(Protocol, text, _sharedProtocols ? "pending" : null, out var state) ||
                    _sharedProtocols && ProtocolConsoleEventParser.TryParse(CallProtocol.H323, text, null, out state)))
            {
                if (state is CallEnded or CallFailed) Interlocked.Exchange(ref _callAwaitingTerminal, 0);
                else if (state is CallConnected or IncomingCall) Interlocked.Exchange(ref _callAwaitingTerminal, 1);
            }
            LineReceived?.Invoke(this, message);
            if (message.TryGetLegacyText(out var readyText))
            {
                lock (_readyLock)
                    if (_waitingReady.RemoveWhere(prefix => readyText.StartsWith(prefix, StringComparison.Ordinal)) > 0 && _waitingReady.Count == 0)
                        _startupReady?.TrySetResult();
            }
        }
    }

    internal static ReadOnlyMemory<byte> NormalizeStandardStreamLine(byte[] bytes)
    {
        if (bytes.Length < 2 || bytes[^2] != (byte)'\r' || bytes[^1] != (byte)'\n')
        {
            return bytes;
        }

        bytes[^2] = (byte)'\n';
        return bytes.AsMemory(0, bytes.Length - 1);
    }
}
