using System.IO;
using System.Runtime.InteropServices;
using NAudio.CoreAudioApi;
using NAudio.Wave;

namespace TeleStudio.Desktop.Audio;

public sealed class WasapiAudioDiagnostics : IAudioDiagnostics
{
    private static readonly TimeSpan RequiredDuration = TimeSpan.FromSeconds(3);
    private readonly IAudioPlatform _platform;
    private readonly IAudioFileStore _files;
    private readonly string _temporaryDirectory;
    private readonly string _wavePath;
    private readonly Func<TimeSpan, CancellationToken, Task> _delay;
    private readonly SemaphoreSlim _operationGate = new(1, 1);
    private bool _disposed;

    public WasapiAudioDiagnostics()
        : this(
            new NAudioPlatform(),
            new LocalAudioFileStore(),
            Path.Combine(Path.GetTempPath(), "TeleStudio"),
            Task.Delay)
    {
    }

    internal WasapiAudioDiagnostics(
        IAudioPlatform platform,
        IAudioFileStore files,
        string temporaryDirectory,
        Func<TimeSpan, CancellationToken, Task> delay)
    {
        _platform = platform;
        _files = files;
        _temporaryDirectory = temporaryDirectory;
        _wavePath = Path.Combine(temporaryDirectory, "mic-test.wav");
        _delay = delay;
    }

    public IReadOnlyList<AudioDeviceInfo> GetCaptureDevices()
    {
        ThrowIfDisposed();
        return _platform.Enumerate(AudioEndpointRole.Capture)
            .Select(endpoint => new AudioDeviceInfo(endpoint.Id, endpoint.Name, endpoint.IsDefault))
            .ToArray();
    }

    public async Task<MicrophoneTestResult> RecordTestAsync(
        string? deviceId,
        TimeSpan duration,
        CancellationToken cancellationToken)
    {
        ThrowIfDisposed();
        if (duration != RequiredDuration)
        {
            throw new ArgumentOutOfRangeException(
                nameof(duration),
                duration,
                "The local microphone diagnostic records for exactly three seconds.");
        }

        await _operationGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        IAudioCaptureSession? capture = null;
        IAudioWaveWriter? writer = null;
        EventHandler<AudioDataEventArgs>? dataHandler = null;
        EventHandler<AudioSessionStoppedEventArgs>? stoppedHandler = null;
        var succeeded = false;

        try
        {
            ThrowIfDisposed();
            _files.CreateDirectory(_temporaryDirectory);
            DeleteWaveIfPresent();

            var endpoint = ResolveEndpoint(
                _platform.Enumerate(AudioEndpointRole.Capture),
                deviceId,
                AudioEndpointRole.Capture);
            capture = _platform.CreateCapture(endpoint);
            var captureDeviceId = capture.DeviceId;
            var captureDeviceName = capture.DeviceName;
            writer = _platform.CreateWriter(_wavePath, capture.WaveFormat);
            var samples = new List<float>();
            var stopped = new TaskCompletionSource<Exception?>(TaskCreationOptions.RunContinuationsAsynchronously);
            Exception? dataFailure = null;

            dataHandler = (_, eventArgs) =>
            {
                try
                {
                    writer.Write(eventArgs.Buffer.Span);
                    var decoded = DecodeSamples(eventArgs.Buffer.Span, capture.WaveFormat);
                    lock (samples)
                    {
                        samples.AddRange(decoded);
                    }
                }
                catch (Exception error)
                {
                    dataFailure = error;
                    try
                    {
                        capture.Stop();
                    }
                    catch (Exception stopError)
                    {
                        stopped.TrySetResult(new AggregateException(error, stopError));
                    }
                }
            };
            stoppedHandler = (_, eventArgs) => stopped.TrySetResult(eventArgs.Exception);
            capture.DataAvailable += dataHandler;
            capture.RecordingStopped += stoppedHandler;
            capture.Start();

            var elapsed = _delay(duration, CancellationToken.None);
            var cancelled = Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken);
            var winner = await Task.WhenAny(stopped.Task, elapsed, cancelled).ConfigureAwait(false);

            if (winner == cancelled)
            {
                TryStop(capture);
                await stopped.Task.ConfigureAwait(false);
                throw new OperationCanceledException(cancellationToken);
            }

            if (winner == stopped.Task)
            {
                var earlyError = await stopped.Task.ConfigureAwait(false);
                throw dataFailure ?? earlyError ??
                    new InvalidOperationException("Microphone capture stopped before the required three seconds elapsed.");
            }

            TryStop(capture);
            var stopFailure = await stopped.Task.ConfigureAwait(false);
            if (dataFailure is not null)
            {
                throw new InvalidDataException("Microphone samples could not be decoded.", dataFailure);
            }
            if (stopFailure is not null)
            {
                throw new InvalidOperationException("Microphone capture failed.", stopFailure);
            }

            float[] measured;
            lock (samples)
            {
                measured = samples.ToArray();
            }
            var levels = MeasureSamples(measured);
            succeeded = true;
            return new(
                captureDeviceId,
                captureDeviceName,
                levels.PeakDbFs,
                levels.RmsDbFs,
                levels.NonSilent,
                _wavePath);
        }
        finally
        {
            Exception? cleanupFailure = null;
            if (capture is not null)
            {
                if (dataHandler is not null)
                {
                    cleanupFailure = CaptureCleanupFailure(
                        cleanupFailure,
                        () => capture.DataAvailable -= dataHandler);
                }
                if (stoppedHandler is not null)
                {
                    cleanupFailure = CaptureCleanupFailure(
                        cleanupFailure,
                        () => capture.RecordingStopped -= stoppedHandler);
                }
            }
            if (writer is not null)
            {
                cleanupFailure = CaptureCleanupFailure(cleanupFailure, writer.Dispose);
            }
            if (capture is not null)
            {
                cleanupFailure = CaptureCleanupFailure(cleanupFailure, capture.Dispose);
            }
            if (!succeeded || cleanupFailure is not null)
            {
                cleanupFailure = CaptureCleanupFailure(cleanupFailure, DeleteWaveIfPresent);
            }
            _operationGate.Release();
            if (cleanupFailure is not null)
            {
                throw new IOException("Microphone diagnostic cleanup failed.", cleanupFailure);
            }
        }
    }

    public async Task PlayAsync(string wavePath, string? speakerId, CancellationToken cancellationToken)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(wavePath);
        ThrowIfDisposed();
        await _operationGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        IAudioPlaybackSession? playback = null;
        EventHandler<AudioSessionStoppedEventArgs>? stoppedHandler = null;
        Exception? primaryFailure = null;

        try
        {
            ThrowIfDisposed();
            if (!_files.Exists(wavePath))
            {
                throw new FileNotFoundException("The completed microphone test WAV does not exist.", wavePath);
            }

            var endpoint = ResolveEndpoint(
                _platform.Enumerate(AudioEndpointRole.Render),
                speakerId,
                AudioEndpointRole.Render);
            playback = _platform.CreatePlayback(endpoint, wavePath);
            var stopped = new TaskCompletionSource<Exception?>(TaskCreationOptions.RunContinuationsAsynchronously);
            stoppedHandler = (_, eventArgs) => stopped.TrySetResult(eventArgs.Exception);
            playback.PlaybackStopped += stoppedHandler;
            playback.Start();

            var cancelled = Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken);
            var winner = await Task.WhenAny(stopped.Task, cancelled).ConfigureAwait(false);
            if (winner == cancelled)
            {
                TryStop(playback);
                await stopped.Task.ConfigureAwait(false);
                throw new OperationCanceledException(cancellationToken);
            }

            var playbackFailure = await stopped.Task.ConfigureAwait(false);
            if (playbackFailure is not null)
            {
                throw new InvalidOperationException("Local playback failed.", playbackFailure);
            }
        }
        catch (Exception error)
        {
            primaryFailure = error;
            throw;
        }
        finally
        {
            Exception? cleanupFailure = null;
            if (playback is not null && stoppedHandler is not null)
            {
                cleanupFailure = CaptureCleanupFailure(
                    cleanupFailure,
                    () => playback.PlaybackStopped -= stoppedHandler);
            }
            if (playback is not null)
            {
                cleanupFailure = CaptureCleanupFailure(cleanupFailure, playback.Dispose);
            }
            _operationGate.Release();
            if (cleanupFailure is not null)
            {
                if (primaryFailure is OperationCanceledException cancellation)
                {
                    throw new OperationCanceledException(
                        "Local playback was canceled and cleanup also failed.",
                        new AggregateException(primaryFailure, cleanupFailure),
                        cancellation.CancellationToken);
                }
                if (primaryFailure is not null)
                {
                    throw new AggregateException(
                        "Local playback failed and cleanup also failed.",
                        primaryFailure,
                        cleanupFailure);
                }

                throw new IOException("Local playback cleanup failed.", cleanupFailure);
            }
        }
    }

    public async ValueTask DisposeAsync()
    {
        if (_disposed) return;
        await _operationGate.WaitAsync().ConfigureAwait(false);
        try
        {
            if (_disposed) return;
            DeleteWaveIfPresent();
            _disposed = true;
        }
        finally
        {
            _operationGate.Release();
        }
    }

    internal static AudioLevels MeasureSamples(ReadOnlySpan<float> samples)
    {
        if (samples.IsEmpty)
        {
            throw new InvalidDataException("No microphone samples were decoded.");
        }

        double peak = 0;
        double squareSum = 0;
        foreach (var raw in samples)
        {
            if (!float.IsFinite(raw))
            {
                throw new InvalidDataException("Microphone samples must be finite.");
            }
            var sample = Math.Clamp((double)raw, -1, 1);
            peak = Math.Max(peak, Math.Abs(sample));
            squareSum += sample * sample;
        }

        var rms = Math.Sqrt(squareSum / samples.Length);
        var peakDbFs = ToDbFs(peak);
        var rmsDbFs = ToDbFs(rms);
        return new(peakDbFs, rmsDbFs, IsNonSilent(rmsDbFs, peakDbFs));
    }

    internal static bool IsNonSilent(double rmsDbFs, double peakDbFs) =>
        rmsDbFs > -55 && peakDbFs > -45;

    internal static float[] DecodeSamples(ReadOnlySpan<byte> bytes, WaveFormat waveFormat)
    {
        ArgumentNullException.ThrowIfNull(waveFormat);
        if (waveFormat is WaveFormatExtensible extensible)
        {
            waveFormat = extensible.ToStandardWaveFormat();
        }

        if (waveFormat.Channels <= 0 || waveFormat.BlockAlign <= 0 || bytes.IsEmpty)
        {
            throw new InvalidDataException("The capture format or sample buffer is invalid.");
        }
        if (bytes.Length % waveFormat.BlockAlign != 0)
        {
            throw new InvalidDataException("The capture buffer contains an incomplete audio frame.");
        }

        var bytesPerSample = waveFormat.BitsPerSample / 8;
        if (bytesPerSample <= 0 || waveFormat.BlockAlign != bytesPerSample * waveFormat.Channels)
        {
            throw new InvalidDataException("The capture format has inconsistent frame geometry.");
        }

        var samples = new float[bytes.Length / bytesPerSample];
        for (var index = 0; index < samples.Length; index++)
        {
            var sampleBytes = bytes.Slice(index * bytesPerSample, bytesPerSample);
            samples[index] = waveFormat.Encoding switch
            {
                WaveFormatEncoding.Pcm => DecodePcm(sampleBytes, waveFormat.BitsPerSample),
                WaveFormatEncoding.IeeeFloat => DecodeFloat(sampleBytes, waveFormat.BitsPerSample),
                _ => throw new NotSupportedException(
                    $"Capture format {waveFormat.Encoding} is not supported for microphone diagnostics.")
            };
            if (!float.IsFinite(samples[index]))
            {
                throw new InvalidDataException("The decoded microphone sample is not finite.");
            }
            samples[index] = Math.Clamp(samples[index], -1f, 1f);
        }
        return samples;
    }

    internal static AudioEndpointDescriptor ResolveEndpoint(
        IReadOnlyList<AudioEndpointDescriptor> endpoints,
        string? selectedId,
        AudioEndpointRole role)
    {
        var endpoint = selectedId is null
            ? endpoints.FirstOrDefault(candidate => candidate.IsDefault)
            : endpoints.FirstOrDefault(candidate => string.Equals(candidate.Id, selectedId, StringComparison.Ordinal));

        if (endpoint is not null) return endpoint;
        var direction = role == AudioEndpointRole.Capture ? "capture" : "render";
        throw new InvalidOperationException(selectedId is null
            ? $"No active Multimedia default {direction} endpoint is available. Re-enumerate audio devices and check Windows audio settings."
            : $"The selected {direction} endpoint is unavailable or stale. Re-enumerate audio devices and select it again.");
    }

    private static double ToDbFs(double value) => 20 * Math.Log10(Math.Max(value, 1e-8));

    private static float DecodePcm(ReadOnlySpan<byte> bytes, int bitsPerSample) => bitsPerSample switch
    {
        8 => (bytes[0] - 128) / 128f,
        16 => BitConverter.ToInt16(bytes) / 32768f,
        24 => DecodeInt24(bytes) / 8388608f,
        32 => BitConverter.ToInt32(bytes) / 2147483648f,
        _ => throw new NotSupportedException($"{bitsPerSample}-bit PCM capture is not supported.")
    };

    private static float DecodeFloat(ReadOnlySpan<byte> bytes, int bitsPerSample) => bitsPerSample switch
    {
        32 => BitConverter.ToSingle(bytes),
        64 => (float)BitConverter.ToDouble(bytes),
        _ => throw new NotSupportedException($"{bitsPerSample}-bit IEEE-float capture is not supported.")
    };

    private static int DecodeInt24(ReadOnlySpan<byte> bytes)
    {
        var value = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16);
        return (value & 0x800000) == 0 ? value : value | unchecked((int)0xFF000000);
    }

    private static void TryStop(IAudioCaptureSession capture)
    {
        try { capture.Stop(); }
        catch (InvalidOperationException) { }
    }

    private static void TryStop(IAudioPlaybackSession playback)
    {
        try { playback.Stop(); }
        catch (InvalidOperationException) { }
    }

    private void DeleteWaveIfPresent()
    {
        if (_files.Exists(_wavePath)) _files.Delete(_wavePath);
    }

    private static Exception? CaptureCleanupFailure(Exception? existing, Action cleanup)
    {
        try
        {
            cleanup();
            return existing;
        }
        catch (Exception error)
        {
            return existing is null ? error : new AggregateException(existing, error);
        }
    }

    private void ThrowIfDisposed() => ObjectDisposedException.ThrowIf(_disposed, this);
}

internal sealed record AudioLevels(double PeakDbFs, double RmsDbFs, bool NonSilent);
internal sealed record AudioEndpointDescriptor(string Id, string Name, bool IsDefault);
internal enum AudioEndpointRole { Capture, Render }
internal sealed class AudioDataEventArgs(ReadOnlyMemory<byte> buffer) : EventArgs
{
    public ReadOnlyMemory<byte> Buffer { get; } = buffer;
}

internal sealed class AudioSessionStoppedEventArgs(Exception? exception) : EventArgs
{
    public Exception? Exception { get; } = exception;
}

internal interface IAudioFileStore
{
    bool Exists(string path);
    void CreateDirectory(string path);
    void Delete(string path);
}

internal interface IAudioPlatform
{
    IReadOnlyList<AudioEndpointDescriptor> Enumerate(AudioEndpointRole role);
    IAudioCaptureSession CreateCapture(AudioEndpointDescriptor endpoint);
    IAudioWaveWriter CreateWriter(string path, WaveFormat format);
    IAudioPlaybackSession CreatePlayback(AudioEndpointDescriptor endpoint, string wavePath);
}

internal interface IAudioCaptureSession : IDisposable
{
    string DeviceId { get; }
    string DeviceName { get; }
    WaveFormat WaveFormat { get; }
    event EventHandler<AudioDataEventArgs>? DataAvailable;
    event EventHandler<AudioSessionStoppedEventArgs>? RecordingStopped;
    void Start();
    void Stop();
}

internal interface IAudioWaveWriter : IDisposable
{
    void Write(ReadOnlySpan<byte> buffer);
}

internal interface IAudioPlaybackSession : IDisposable
{
    event EventHandler<AudioSessionStoppedEventArgs>? PlaybackStopped;
    void Start();
    void Stop();
}

internal sealed class LocalAudioFileStore : IAudioFileStore
{
    public bool Exists(string path) => File.Exists(path);
    public void CreateDirectory(string path) => Directory.CreateDirectory(path);
    public void Delete(string path) => File.Delete(path);
}

internal sealed class NAudioPlatform : IAudioPlatform
{
    public IReadOnlyList<AudioEndpointDescriptor> Enumerate(AudioEndpointRole role)
    {
        using var enumerator = new MMDeviceEnumerator();
        var flow = role == AudioEndpointRole.Capture ? DataFlow.Capture : DataFlow.Render;
        string? defaultId = null;
        try
        {
            using var defaultDevice = enumerator.GetDefaultAudioEndpoint(flow, Role.Multimedia);
            defaultId = defaultDevice.ID;
        }
        catch (COMException) { }
        catch (InvalidOperationException) { }

        var devices = enumerator.EnumerateAudioEndPoints(flow, DeviceState.Active);
        var endpoints = new List<AudioEndpointDescriptor>(devices.Count);
        foreach (var device in devices)
        {
            using (device)
            {
                endpoints.Add(new AudioEndpointDescriptor(
                device.ID,
                device.FriendlyName,
                string.Equals(device.ID, defaultId, StringComparison.Ordinal)));
            }
        }
        return endpoints;
    }

    public IAudioCaptureSession CreateCapture(AudioEndpointDescriptor endpoint)
    {
        try
        {
            using var enumerator = new MMDeviceEnumerator();
            var device = enumerator.GetDevice(endpoint.Id);
            try
            {
                return new NAudioCaptureSession(device, endpoint.Name);
            }
            catch
            {
                device.Dispose();
                throw;
            }
        }
        catch (Exception error) when (error is COMException or InvalidOperationException)
        {
            throw new InvalidOperationException(
                "The selected capture endpoint is no longer available. Re-enumerate audio devices and select it again.",
                error);
        }
    }

    public IAudioWaveWriter CreateWriter(string path, WaveFormat format) =>
        new NAudioWaveWriter(path, format);

    public IAudioPlaybackSession CreatePlayback(AudioEndpointDescriptor endpoint, string wavePath)
    {
        try
        {
            using var enumerator = new MMDeviceEnumerator();
            var device = enumerator.GetDevice(endpoint.Id);
            try
            {
                return new NAudioPlaybackSession(device, wavePath);
            }
            catch
            {
                device.Dispose();
                throw;
            }
        }
        catch (Exception error) when (error is COMException or InvalidOperationException)
        {
            throw new InvalidOperationException(
                "The selected render endpoint is no longer available. Re-enumerate audio devices and select it again.",
                error);
        }
    }
}

internal sealed class NAudioCaptureSession : IAudioCaptureSession
{
    private readonly MMDevice _device;
    private readonly WasapiCapture _capture;

    public NAudioCaptureSession(MMDevice device, string deviceName)
    {
        _device = device;
        DeviceName = deviceName;
        var capture = new WasapiCapture(device, useEventSync: true, audioBufferMillisecondsLength: 100)
        {
            ShareMode = AudioClientShareMode.Shared
        };
        try
        {
            capture.DataAvailable += OnDataAvailable;
            capture.RecordingStopped += OnRecordingStopped;
            _capture = capture;
        }
        catch
        {
            capture.Dispose();
            throw;
        }
    }

    public string DeviceId => _device.ID;
    public string DeviceName { get; }
    public WaveFormat WaveFormat => _capture.WaveFormat;
    public event EventHandler<AudioDataEventArgs>? DataAvailable;
    public event EventHandler<AudioSessionStoppedEventArgs>? RecordingStopped;
    public void Start() => _capture.StartRecording();
    public void Stop() => _capture.StopRecording();
    public void Dispose()
    {
        _capture.DataAvailable -= OnDataAvailable;
        _capture.RecordingStopped -= OnRecordingStopped;
        try
        {
            _capture.Dispose();
        }
        finally
        {
            _device.Dispose();
        }
    }

    private void OnDataAvailable(object? sender, WaveInEventArgs eventArgs)
    {
        var bytes = new byte[eventArgs.BytesRecorded];
        eventArgs.Buffer.AsSpan(0, eventArgs.BytesRecorded).CopyTo(bytes);
        DataAvailable?.Invoke(this, new(bytes));
    }

    private void OnRecordingStopped(object? sender, StoppedEventArgs eventArgs) =>
        RecordingStopped?.Invoke(this, new(eventArgs.Exception));
}

internal sealed class NAudioWaveWriter : IAudioWaveWriter
{
    private readonly WaveFileWriter _writer;
    public NAudioWaveWriter(string path, WaveFormat format) => _writer = new(path, format);
    public void Write(ReadOnlySpan<byte> buffer) => _writer.Write(buffer);
    public void Dispose() => _writer.Dispose();
}

internal sealed class NAudioPlaybackSession : IAudioPlaybackSession
{
    private readonly MMDevice _device;
    private readonly WaveFileReader _reader;
    private readonly WasapiOut _output;

    public NAudioPlaybackSession(MMDevice device, string wavePath)
    {
        _device = device;
        WaveFileReader? reader = null;
        WasapiOut? output = null;
        try
        {
            reader = new WaveFileReader(wavePath);
            output = new WasapiOut(device, AudioClientShareMode.Shared, useEventSync: true, latency: 100);
            output.Init(reader);
            output.PlaybackStopped += OnPlaybackStopped;
            _reader = reader;
            _output = output;
        }
        catch
        {
            output?.Dispose();
            reader?.Dispose();
            throw;
        }
    }

    public event EventHandler<AudioSessionStoppedEventArgs>? PlaybackStopped;
    public void Start() => _output.Play();
    public void Stop() => _output.Stop();
    public void Dispose()
    {
        _output.PlaybackStopped -= OnPlaybackStopped;
        try
        {
            _output.Dispose();
        }
        finally
        {
            try
            {
                _reader.Dispose();
            }
            finally
            {
                _device.Dispose();
            }
        }
    }

    private void OnPlaybackStopped(object? sender, StoppedEventArgs eventArgs) =>
        PlaybackStopped?.Invoke(this, new(eventArgs.Exception));
}
