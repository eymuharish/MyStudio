using NAudio.Wave;
using System.IO;
using TeleStudio.Contracts;

namespace TeleStudio.Desktop;

public enum CallTone { Dialing, Ended }

public interface ICallToneOutput
{
    Task PlayAsync(CallTone tone, CancellationToken cancellationToken);
}

// Called on the UI dispatcher. Sounds never determine the protocol call state.
public sealed class CallToneController(ICallToneOutput output) : IDisposable
{
    private CancellationTokenSource? _playback;
    private bool _active;
    private bool _ringStarted;
    private bool _disposed;
    private Task _ending = Task.CompletedTask;

    public void Observe(EngineEvent value)
    {
        if (_disposed) return;
        switch(value)
        {
            case IncomingCall:
                _active = true;
                _ringStarted = true; // Incoming ringing is not outbound ringback.
                break;
            case CallProgress when !_ringStarted:
                _active = true;
                _ringStarted = true;
                _ = PlayAsync(CallTone.Dialing);
                break;
            case CallConnected:
                _active = true;
                _ringStarted = true;
                CancelPlayback();
                break;
            case CallEnding or CallEnded or CallFailed:
                _ = EndAsync();
                break;
        }
    }

    public Task EndAsync()
    {
        if (_disposed) return Task.CompletedTask;
        if (!_active) return _ending;
        _active = false;
        _ringStarted = false;
        _ending = PlayAsync(CallTone.Ended);
        return _ending;
    }

    private async Task PlayAsync(CallTone tone)
    {
        CancelPlayback();
        var cancellation = _playback = new CancellationTokenSource();
        try { await output.PlayAsync(tone, cancellation.Token); }
        catch(OperationCanceledException) { }
        catch(Exception error)
        {
            // Losing the output device must not prevent dialing or shutdown.
            Runtime.StartupErrorLog.TryWriteOperation(error,"Local call tone",out _);
        }
    }
    private void CancelPlayback()
    {
        _playback?.Cancel();
        _playback?.Dispose();
        _playback = null;
    }
    public void Dispose() { _disposed = true; CancelPlayback(); }
}

public static class CallTonePcm
{
    public const int SampleRate = 24000;
    public static float[] Create(CallTone tone)
    {
        var duration = tone == CallTone.Dialing ? 2.0 : .45;
        var samples = new float[(int)(SampleRate * duration)];
        for(var i=0;i<samples.Length;i++)
        {
            var t=(double)i/SampleRate;
            // Two soft ring pulses; a short descending pair signals call end.
            var pulse=tone==CallTone.Dialing ? t % 1.0 : t;
            var length=tone==CallTone.Dialing ? .65 : duration;
            if(pulse>=length) continue;
            var envelope=Math.Min(1,Math.Min(pulse/.015,(length-pulse)/.025));
            var frequency=tone==CallTone.Dialing ? 440 : (t<.20 ? 520 : 360);
            samples[i]=(float)(.09*envelope*(Math.Sin(2*Math.PI*frequency*t)+Math.Sin(2*Math.PI*(frequency+40)*t)));
        }
        return samples;
    }
}

public sealed class LocalCallToneOutput : ICallToneOutput
{
    public async Task PlayAsync(CallTone tone,CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var samples=CallTonePcm.Create(tone);
        var bytes=new byte[samples.Length*sizeof(float)];
        Buffer.BlockCopy(samples,0,bytes,0,bytes.Length);
        using var memory=new MemoryStream(bytes,false);
        using var stream=new RawSourceWaveStream(memory,WaveFormat.CreateIeeeFloatWaveFormat(CallTonePcm.SampleRate,1));
        using var device=new WaveOutEvent { DesiredLatency=70 };
        var finished=new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        device.PlaybackStopped+=(_,args)=>
        {
            if(args.Exception is {} error) finished.TrySetException(error);
            else finished.TrySetResult();
        };
        device.Init(stream);
        using var registration=cancellationToken.Register(()=>device.Stop());
        try
        {
            cancellationToken.ThrowIfCancellationRequested();
            device.Play();
            await finished.Task.WaitAsync(TimeSpan.FromSeconds(4),cancellationToken);
        }
        finally { device.Stop(); }
    }
}
