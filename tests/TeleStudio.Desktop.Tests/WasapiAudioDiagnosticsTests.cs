using System.Text;
using NAudio.Wave;
using TeleStudio.Desktop.Audio;

namespace TeleStudio.Desktop.Tests;

public sealed class WasapiAudioDiagnosticsTests
{
    [Fact]
    public void Audio_device_displays_only_its_friendly_name()
    {
        var device = new AudioDeviceInfo(
            "{0.0.1.00000000}.{82d25679-05a3-4461-993a-976439b526e3}",
            "ميكروفونات ستريو Surface",
            true);

        Assert.Equal("ميكروفونات ستريو Surface", device.ToString());
    }

    [Fact]
    public void Meter_returns_finite_floor_for_silence()
    {
        var levels = WasapiAudioDiagnostics.MeasureSamples([0f, 0f, 0f]);

        Assert.Equal(-160, levels.PeakDbFs);
        Assert.Equal(-160, levels.RmsDbFs);
        Assert.False(levels.NonSilent);
    }

    [Fact]
    public void Meter_measures_minus_six_dbfs_sine()
    {
        var amplitude = MathF.Pow(10, -6f / 20f);
        var samples = Enumerable.Range(0, 4_800)
            .Select(index => amplitude * MathF.Sin(2 * MathF.PI * index / 48))
            .ToArray();

        var levels = WasapiAudioDiagnostics.MeasureSamples(samples);

        Assert.InRange(levels.PeakDbFs, -6.01, -5.99);
        Assert.InRange(levels.RmsDbFs, -9.02, -9.00);
        Assert.True(levels.NonSilent);
    }

    [Fact]
    public void Meter_clamps_clipping_and_counts_all_channels()
    {
        var levels = WasapiAudioDiagnostics.MeasureSamples([2f, -2f, 0f, 0f]);

        Assert.Equal(0, levels.PeakDbFs, 6);
        Assert.Equal(-3.0103, levels.RmsDbFs, 3);
    }

    [Theory]
    [InlineData(-55.0, -44.0, false)]
    [InlineData(-54.999, -44.0, true)]
    [InlineData(-40.0, -45.0, false)]
    [InlineData(-40.0, -44.999, true)]
    public void Non_silent_thresholds_are_strict(double rms, double peak, bool expected) =>
        Assert.Equal(expected, WasapiAudioDiagnostics.IsNonSilent(rms, peak));

    [Fact]
    public void Meter_rejects_non_finite_or_missing_samples()
    {
        Assert.Throws<InvalidDataException>(() => WasapiAudioDiagnostics.MeasureSamples([]));
        Assert.Throws<InvalidDataException>(() => WasapiAudioDiagnostics.MeasureSamples([float.NaN]));
        Assert.Throws<InvalidDataException>(() => WasapiAudioDiagnostics.MeasureSamples([float.PositiveInfinity]));
    }

    [Fact]
    public void Decoder_normalizes_equivalent_pcm16_and_float_stereo_frames()
    {
        var pcm = new byte[] { 0x00, 0x40, 0x00, 0xC0 };
        var floating = new byte[8];
        BitConverter.GetBytes(0.5f).CopyTo(floating, 0);
        BitConverter.GetBytes(-0.5f).CopyTo(floating, 4);

        var pcmSamples = WasapiAudioDiagnostics.DecodeSamples(pcm, new WaveFormat(48_000, 16, 2));
        var floatSamples = WasapiAudioDiagnostics.DecodeSamples(
            floating, WaveFormat.CreateIeeeFloatWaveFormat(48_000, 2));

        Assert.Equal(floatSamples, pcmSamples, new FloatComparer(0.0001f));
    }

    [Fact]
    public void Decoder_rejects_incomplete_frames_and_unsupported_formats()
    {
        Assert.Throws<InvalidDataException>(() =>
            WasapiAudioDiagnostics.DecodeSamples([0, 1, 2], new WaveFormat(48_000, 16, 2)));
        var alaw = WaveFormat.CreateCustomFormat(WaveFormatEncoding.ALaw, 8_000, 1, 8_000, 1, 8);
        Assert.Throws<NotSupportedException>(() => WasapiAudioDiagnostics.DecodeSamples([0], alaw));
    }

    [Fact]
    public void Endpoint_resolution_uses_default_for_null_and_exact_id_without_fallback()
    {
        AudioEndpointDescriptor[] endpoints =
        [
            new("capture-a", "Mic A", false),
            new("capture-b", "Mic B", true)
        ];

        Assert.Equal("capture-b", WasapiAudioDiagnostics.ResolveEndpoint(endpoints, null, AudioEndpointRole.Capture).Id);
        Assert.Equal("capture-a", WasapiAudioDiagnostics.ResolveEndpoint(endpoints, "capture-a", AudioEndpointRole.Capture).Id);
        Assert.Throws<InvalidOperationException>(() =>
            WasapiAudioDiagnostics.ResolveEndpoint(endpoints, "stale", AudioEndpointRole.Capture));
        Assert.Throws<InvalidOperationException>(() =>
            WasapiAudioDiagnostics.ResolveEndpoint([], null, AudioEndpointRole.Render));
    }

    [Fact]
    public async Task Capture_enumeration_is_safe_when_no_endpoint_exists()
    {
        var fixture = new Fixture();
        fixture.Platform.CaptureEndpoints.Clear();
        await using var diagnostics = fixture.CreateDiagnostics();

        Assert.Empty(diagnostics.GetCaptureDevices());
    }

    [Fact]
    public async Task Recording_requires_exactly_three_seconds()
    {
        var fixture = new Fixture();
        await using var diagnostics = fixture.CreateDiagnostics();

        await Assert.ThrowsAsync<ArgumentOutOfRangeException>(() =>
            diagnostics.RecordTestAsync(null, TimeSpan.FromSeconds(2.99), CancellationToken.None));
    }

    [Fact]
    public async Task Successful_recording_replaces_previous_wave_and_detaches_handlers()
    {
        var fixture = new Fixture();
        fixture.Files.Files.Add(fixture.WavePath);
        await using var diagnostics = fixture.CreateDiagnostics();

        var task = diagnostics.RecordTestAsync("capture-a", TimeSpan.FromSeconds(3), CancellationToken.None);
        await fixture.Platform.Capture.Started.Task;
        fixture.Platform.Capture.Emit(Pcm16(0.5f, -0.5f));
        fixture.Delay.Complete();
        var result = await task;

        Assert.Equal("capture-a", result.DeviceId);
        Assert.Equal("Surface Microphone", result.DeviceName);
        Assert.True(result.NonSilent);
        Assert.Equal(fixture.WavePath, result.WavePath);
        Assert.Equal(1, fixture.Files.DeleteCalls);
        Assert.Contains(fixture.WavePath, fixture.Files.Files);
        Assert.Equal(0, fixture.Platform.Capture.DataHandlerCount);
        Assert.Equal(0, fixture.Platform.Capture.StoppedHandlerCount);
        Assert.True(fixture.Platform.Capture.Disposed);
        Assert.True(fixture.Platform.Writer.Disposed);
    }

    [Fact]
    public async Task Recording_snapshots_device_identity_before_capture_stops()
    {
        var fixture = new Fixture();
        fixture.Platform.Capture.IdentityUnavailableAfterStop = true;
        await using var diagnostics = fixture.CreateDiagnostics();

        var task = diagnostics.RecordTestAsync("capture-a", TimeSpan.FromSeconds(3), CancellationToken.None);
        await fixture.Platform.Capture.Started.Task;
        fixture.Platform.Capture.Emit(Pcm16(0.5f, -0.5f));
        fixture.Delay.Complete();
        var result = await task;

        Assert.Equal("capture-a", result.DeviceId);
        Assert.Equal("Surface Microphone", result.DeviceName);
    }

    [Fact]
    public async Task Early_stop_and_device_error_are_inconclusive_and_delete_partial_wave()
    {
        var early = new Fixture();
        early.Platform.Capture.StopOnStart();
        await using (var diagnostics = early.CreateDiagnostics())
        {
            await Assert.ThrowsAsync<InvalidOperationException>(() =>
                diagnostics.RecordTestAsync(null, TimeSpan.FromSeconds(3), CancellationToken.None));
        }
        Assert.DoesNotContain(early.WavePath, early.Files.Files);

        var failed = new Fixture();
        await using (var diagnostics = failed.CreateDiagnostics())
        {
            var task = diagnostics.RecordTestAsync(null, TimeSpan.FromSeconds(3), CancellationToken.None);
            await failed.Platform.Capture.Started.Task;
            failed.Platform.Capture.Fail(new InvalidOperationException("endpoint lost"));
            var error = await Assert.ThrowsAsync<InvalidOperationException>(() => task);
            Assert.Contains("endpoint lost", error.ToString(), StringComparison.OrdinalIgnoreCase);
        }
        Assert.DoesNotContain(failed.WavePath, failed.Files.Files);
    }

    [Fact]
    public async Task Cancellation_waits_for_teardown_and_deletes_partial_wave()
    {
        var fixture = new Fixture();
        await using var diagnostics = fixture.CreateDiagnostics();
        using var cancellation = new CancellationTokenSource();

        var task = diagnostics.RecordTestAsync(null, TimeSpan.FromSeconds(3), cancellation.Token);
        await fixture.Platform.Capture.Started.Task;
        cancellation.Cancel();

        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => task);
        Assert.True(fixture.Platform.Capture.Disposed);
        Assert.DoesNotContain(fixture.WavePath, fixture.Files.Files);
    }

    [Fact]
    public async Task Playback_selects_exact_render_device_and_honors_cancellation()
    {
        var fixture = new Fixture();
        fixture.Files.Files.Add(fixture.WavePath);
        await using var diagnostics = fixture.CreateDiagnostics();

        await diagnostics.PlayAsync(fixture.WavePath, "render-a", CancellationToken.None);

        Assert.Equal("render-a", fixture.Platform.SelectedRenderId);
        Assert.True(fixture.Platform.Playback.Disposed);

        var cancelled = new Fixture { AutoCompletePlayback = false };
        cancelled.Files.Files.Add(cancelled.WavePath);
        await using var cancelledDiagnostics = cancelled.CreateDiagnostics();
        using var cancellation = new CancellationTokenSource();
        var task = cancelledDiagnostics.PlayAsync(cancelled.WavePath, null, cancellation.Token);
        await cancelled.Platform.Playback.Started.Task;
        cancellation.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => task);
        Assert.True(cancelled.Platform.Playback.Disposed);
    }

    [Fact]
    public async Task Playback_dispose_failure_after_success_is_actionable_and_releases_gate()
    {
        var fixture = new Fixture();
        fixture.Files.Files.Add(fixture.WavePath);
        fixture.Platform.Playback.DisposeException = new IOException("playback dispose failed");
        var diagnostics = fixture.CreateDiagnostics();

        var error = await Assert.ThrowsAsync<IOException>(() =>
            diagnostics.PlayAsync(fixture.WavePath, null, CancellationToken.None));

        Assert.Contains("playback dispose failed", error.ToString(), StringComparison.OrdinalIgnoreCase);
        Assert.Equal(0, fixture.Platform.Playback.HandlerCount);
        Assert.Equal(1, fixture.Platform.Playback.DisposeCalls);
        await diagnostics.DisposeAsync().AsTask().WaitAsync(TimeSpan.FromSeconds(1));
    }

    [Fact]
    public async Task Playback_dispose_failure_after_cancellation_preserves_cancellation_and_releases_gate()
    {
        var fixture = new Fixture { AutoCompletePlayback = false };
        fixture.Files.Files.Add(fixture.WavePath);
        fixture.Platform.Playback.DisposeException = new IOException("playback dispose failed");
        var diagnostics = fixture.CreateDiagnostics();
        using var cancellation = new CancellationTokenSource();

        var task = diagnostics.PlayAsync(fixture.WavePath, null, cancellation.Token);
        await fixture.Platform.Playback.Started.Task;
        cancellation.Cancel();
        var error = await Assert.ThrowsAnyAsync<OperationCanceledException>(() => task);

        Assert.Equal(cancellation.Token, error.CancellationToken);
        Assert.Contains("playback dispose failed", error.ToString(), StringComparison.OrdinalIgnoreCase);
        Assert.Equal(0, fixture.Platform.Playback.HandlerCount);
        Assert.Equal(1, fixture.Platform.Playback.DisposeCalls);
        await diagnostics.DisposeAsync().AsTask().WaitAsync(TimeSpan.FromSeconds(1));
    }

    [Fact]
    public async Task Shutdown_deletes_wave_missing_is_noop_and_locked_file_is_actionable()
    {
        var fixture = new Fixture();
        fixture.Files.Files.Add(fixture.WavePath);
        var diagnostics = fixture.CreateDiagnostics();
        await diagnostics.DisposeAsync();
        Assert.DoesNotContain(fixture.WavePath, fixture.Files.Files);

        var missing = new Fixture();
        await missing.CreateDiagnostics().DisposeAsync();

        var locked = new Fixture();
        locked.Files.Files.Add(locked.WavePath);
        locked.Files.DeleteException = new IOException("locked");
        var error = await Assert.ThrowsAsync<IOException>(async () => await locked.CreateDiagnostics().DisposeAsync());
        Assert.Contains("locked", error.Message, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public async Task Writer_close_failure_still_disposes_capture_deletes_wave_and_releases_gate()
    {
        var fixture = new Fixture();
        fixture.Platform.Writer.DisposeException = new IOException("writer close failed");
        await using var diagnostics = fixture.CreateDiagnostics();

        var task = diagnostics.RecordTestAsync(null, TimeSpan.FromSeconds(3), CancellationToken.None);
        await fixture.Platform.Capture.Started.Task;
        fixture.Platform.Capture.Emit(Pcm16(0.5f, -0.5f));
        fixture.Delay.Complete();

        var error = await Assert.ThrowsAsync<IOException>(() => task);
        Assert.Contains("writer close failed", error.ToString(), StringComparison.OrdinalIgnoreCase);
        Assert.True(fixture.Platform.Capture.Disposed);
        Assert.DoesNotContain(fixture.WavePath, fixture.Files.Files);
        await diagnostics.DisposeAsync().AsTask().WaitAsync(TimeSpan.FromSeconds(1));
    }

    private static byte[] Pcm16(params float[] samples) => samples
        .SelectMany(sample => BitConverter.GetBytes((short)(sample * short.MaxValue)))
        .ToArray();

    private sealed class Fixture
    {
        public string TempDirectory { get; } = Path.Combine(Path.GetTempPath(), "TeleStudio-tests", Guid.NewGuid().ToString("N"));
        public string WavePath => Path.Combine(TempDirectory, "mic-test.wav");
        public FakeFileStore Files { get; } = new();
        public ControlledDelay Delay { get; } = new();
        public FakePlatform Platform { get; }
        public bool AutoCompletePlayback { get => Platform.Playback.AutoComplete; set => Platform.Playback.AutoComplete = value; }

        public Fixture()
        {
            Platform = new FakePlatform(Files);
        }

        public WasapiAudioDiagnostics CreateDiagnostics() =>
            new(Platform, Files, TempDirectory, Delay.WaitAsync);
    }

    private sealed class ControlledDelay
    {
        private readonly TaskCompletionSource _completion = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public Task WaitAsync(TimeSpan _, CancellationToken __) => _completion.Task;
        public void Complete() => _completion.TrySetResult();
    }

    private sealed class FakeFileStore : IAudioFileStore
    {
        public HashSet<string> Files { get; } = new(StringComparer.OrdinalIgnoreCase);
        public Exception? DeleteException { get; set; }
        public int DeleteCalls { get; private set; }
        public bool Exists(string path) => Files.Contains(path);
        public void CreateDirectory(string path) { }
        public void Delete(string path)
        {
            DeleteCalls++;
            if (DeleteException is not null) throw DeleteException;
            Files.Remove(path);
        }
    }

    private sealed class FakePlatform(FakeFileStore files) : IAudioPlatform
    {
        public List<AudioEndpointDescriptor> CaptureEndpoints { get; } = [new("capture-a", "Surface Microphone", true)];
        public List<AudioEndpointDescriptor> RenderEndpoints { get; } = [new("render-a", "Surface Speakers", true)];
        public FakeCaptureSession Capture { get; } = new();
        public FakePlaybackSession Playback { get; } = new();
        public FakeWaveWriter Writer { get; } = new(files);
        public string? SelectedRenderId { get; private set; }

        public IReadOnlyList<AudioEndpointDescriptor> Enumerate(AudioEndpointRole role) =>
            role == AudioEndpointRole.Capture ? CaptureEndpoints : RenderEndpoints;
        public IAudioCaptureSession CreateCapture(AudioEndpointDescriptor endpoint) => Capture.WithEndpoint(endpoint);
        public IAudioWaveWriter CreateWriter(string path, WaveFormat format) => Writer.Open(path, format);
        public IAudioPlaybackSession CreatePlayback(AudioEndpointDescriptor endpoint, string wavePath)
        {
            SelectedRenderId = endpoint.Id;
            return Playback;
        }
    }

    private sealed class FakeCaptureSession : IAudioCaptureSession
    {
        private EventHandler<AudioDataEventArgs>? _data;
        private EventHandler<AudioSessionStoppedEventArgs>? _stopped;
        private bool _stopOnStart;
        private bool _stoppedCapture;
        private AudioEndpointDescriptor _endpoint = new("capture-a", "Surface Microphone", true);
        public TaskCompletionSource Started { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public WaveFormat WaveFormat { get; } = new(48_000, 16, 1);
        public string DeviceId => IdentityUnavailableAfterStop && _stoppedCapture
            ? throw new InvalidOperationException("capture identity is unavailable after stop")
            : _endpoint.Id;
        public string DeviceName => IdentityUnavailableAfterStop && _stoppedCapture
            ? throw new InvalidOperationException("capture identity is unavailable after stop")
            : _endpoint.Name;
        public bool IdentityUnavailableAfterStop { get; set; }
        public bool Disposed { get; private set; }
        public int DataHandlerCount => _data?.GetInvocationList().Length ?? 0;
        public int StoppedHandlerCount => _stopped?.GetInvocationList().Length ?? 0;
        public event EventHandler<AudioDataEventArgs>? DataAvailable { add => _data += value; remove => _data -= value; }
        public event EventHandler<AudioSessionStoppedEventArgs>? RecordingStopped { add => _stopped += value; remove => _stopped -= value; }
        public FakeCaptureSession WithEndpoint(AudioEndpointDescriptor endpoint) { _endpoint = endpoint; return this; }
        public void Start()
        {
            Started.TrySetResult();
            if (_stopOnStart) _stopped?.Invoke(this, new(null));
        }
        public void Stop() { _stoppedCapture = true; _stopped?.Invoke(this, new(null)); }
        public void Emit(byte[] bytes) => _data?.Invoke(this, new(bytes));
        public void Fail(Exception error) => _stopped?.Invoke(this, new(error));
        public void StopOnStart() => _stopOnStart = true;
        public void Dispose() => Disposed = true;
    }

    private sealed class FakeWaveWriter(FakeFileStore files) : IAudioWaveWriter
    {
        public bool Disposed { get; private set; }
        public Exception? DisposeException { get; set; }
        public IAudioWaveWriter Open(string path, WaveFormat _)
        {
            files.Files.Add(path);
            return this;
        }
        public void Write(ReadOnlySpan<byte> buffer) { }
        public void Dispose()
        {
            Disposed = true;
            if (DisposeException is not null) throw DisposeException;
        }
    }

    private sealed class FakePlaybackSession : IAudioPlaybackSession
    {
        private EventHandler<AudioSessionStoppedEventArgs>? _stopped;
        public TaskCompletionSource Started { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public bool AutoComplete { get; set; } = true;
        public bool Disposed { get; private set; }
        public Exception? DisposeException { get; set; }
        public int DisposeCalls { get; private set; }
        public int HandlerCount => _stopped?.GetInvocationList().Length ?? 0;
        public event EventHandler<AudioSessionStoppedEventArgs>? PlaybackStopped { add => _stopped += value; remove => _stopped -= value; }
        public void Start()
        {
            Started.TrySetResult();
            if (AutoComplete) _stopped?.Invoke(this, new(null));
        }
        public void Stop() => _stopped?.Invoke(this, new(null));
        public void Dispose()
        {
            DisposeCalls++;
            Disposed = true;
            if (DisposeException is not null) throw DisposeException;
        }
    }

    private sealed class FloatComparer(float tolerance) : IEqualityComparer<float>
    {
        public bool Equals(float x, float y) => Math.Abs(x - y) <= tolerance;
        public int GetHashCode(float obj) => obj.GetHashCode();
    }
}
