using TeleStudio.Contracts;

namespace TeleStudio.Desktop;

public sealed record AdaptedVideo(int BitrateKbps, int FramesPerSecond, string Reason, VideoResolution? Resolution = null);

/// <summary>Adapts one outgoing stream; a composed camera/screen is still one stream.</summary>
public sealed class NetworkAdaptationController
{
    private static readonly int[] FrameRates = [2, 5, 10, 15, 24, 30];
    private readonly Queue<(double At, double? Loss, double? Queue)> _samples = new();
    private readonly int _ceilingKbps, _maximumFps, _minimumFps;
    private readonly bool _textScreen;
    private readonly VideoResolution _requestedResolution;
    private int _bitrate, _fps;
    private VideoResolution _resolution;
    private double _lastChange = double.NegativeInfinity;
    private double? _lastSample, _healthySince;

    public NetworkAdaptationController(int ceilingKbps, int maximumFps,
        ScreenContentMode content = ScreenContentMode.Text,
        VideoResolution resolution = VideoResolution.Fhd, bool desktop = true)
    {
        if (ceilingKbps < 128) throw new ArgumentOutOfRangeException(nameof(ceilingKbps));
        if (!FrameRates.Contains(maximumFps) || !desktop && maximumFps < 5) throw new ArgumentOutOfRangeException(nameof(maximumFps));
        if (!Enum.IsDefined(content)) throw new ArgumentOutOfRangeException(nameof(content));
        if (!Enum.IsDefined(resolution)) throw new ArgumentOutOfRangeException(nameof(resolution));
        _ceilingKbps = _bitrate = ceilingKbps;
        _maximumFps = _fps = maximumFps;
        _minimumFps = desktop ? 2 : 5;
        _requestedResolution = _resolution = resolution;
        _textScreen = desktop && content == ScreenContentMode.Text;
    }

    public AdaptedVideo? Observe(double seconds, double? lossPercent, double? queueMs)
    {
        if (!double.IsFinite(seconds) ||
            lossPercent is { } l && (!double.IsFinite(l) || l < 0 || l > 100) ||
            queueMs is { } q && (!double.IsFinite(q) || q < 0))
        {
            ResetObservations();
            return null;
        }
        if (_lastSample is { } previous)
        {
            if (seconds == previous) return null;
            if (seconds < previous)
            {
                ResetObservations();
                _lastChange = double.NegativeInfinity;
            }
            else if (seconds - previous > 2) ResetObservations();
        }
        _lastSample = seconds;
        if (lossPercent is null && queueMs is null)
        {
            ResetObservations();
            return null;
        }
        _samples.Enqueue((seconds, lossPercent, queueMs));
        while (_samples.Count > 0 && seconds - _samples.Peek().At >= 5) _samples.Dequeue();
        if (_samples.Count < 5 || seconds - _samples.Peek().At < 4) return null;
        var knownLoss = _samples.Where(s => s.Loss.HasValue).Select(s => s.Loss!.Value).ToArray();
        double? loss = knownLoss.Length == 0 ? null : knownLoss.Average();
        var knownQueue = _samples.Where(s => s.Queue.HasValue).Select(s => s.Queue!.Value).ToArray();
        double? queued = knownQueue.Length == 0 ? null : knownQueue.Average();
        // Missing loss data cannot establish health, even with an empty local queue.
        if (lossPercent is null || lossPercent >= 1 || loss is null || loss >= 1 || queueMs > 150 || queued > 150) _healthySince = null;
        else _healthySince ??= seconds;
        if (seconds - _lastChange < 10) return null;
        if (loss > 3 || queued > 150) return Reduce(seconds);
        if (_healthySince is { } since && seconds - since >= 30) return Recover(seconds);
        return null;
    }

    private AdaptedVideo? Reduce(double seconds)
    {
        var bitrate = Math.Max(128, (int)Math.Floor(_bitrate * .8));
        var fps = _fps;
        var resolution = _resolution;
        if (_textScreen)
        {
            // Continued pressure at the frame-rate floor may lower text resolution.
            if (fps > _minimumFps) fps = LowerFrameRate(fps);
            else resolution = LowerResolution(resolution);
        }
        else
        {
            resolution = LowerResolution(resolution);
            if (resolution == _resolution) fps = LowerFrameRate(fps);
        }
        if (bitrate == _bitrate && fps == _fps && resolution == _resolution) return null;
        return Apply(seconds, bitrate, fps, resolution, resolution != _resolution
            ? "حماية الصوت: خفض دقة الفيديو بسبب ضغط الشبكة / Protecting audio: reducing video resolution"
            : "حماية الصوت: خفض حمل الفيديو / Protecting audio: video load reduced");
    }

    private AdaptedVideo? Recover(double seconds)
    {
        var bitrate = Math.Min(_ceilingKbps, (int)Math.Ceiling(_bitrate * 1.1));
        var fps = _fps;
        var resolution = _resolution;
        if (bitrate == _ceilingKbps)
        {
            if (_textScreen)
            {
                resolution = HigherResolution(resolution);
                if (resolution == _resolution) fps = HigherFrameRate(fps);
            }
            else
            {
                fps = HigherFrameRate(fps);
                if (fps == _fps) resolution = HigherResolution(resolution);
            }
        }
        if (bitrate == _bitrate && fps == _fps && resolution == _resolution) return null;
        return Apply(seconds, bitrate, fps, resolution,
            "تحسن الاتصال: استعادة الجودة تدريجيًا / Connection improved: restoring quality");
    }

    private AdaptedVideo Apply(double seconds, int bitrate, int fps, VideoResolution resolution, string reason)
    {
        VideoResolution? changedResolution = resolution != _resolution ? resolution : null;
        _bitrate = bitrate;
        _fps = fps;
        _resolution = resolution;
        _lastChange = seconds;
        return new(bitrate, fps, reason, changedResolution);
    }

    private int LowerFrameRate(int current) => FrameRates.LastOrDefault(f => f < current && f >= _minimumFps, current);
    private int HigherFrameRate(int current) => FrameRates.FirstOrDefault(f => f > current && f <= _maximumFps, current);
    private static VideoResolution LowerResolution(VideoResolution current) => current switch
    {
        VideoResolution.Fhd => VideoResolution.Hd,
        VideoResolution.Hd => VideoResolution.Sd,
        _ => current // Auto is unknown: do not invent an applied resolution.
    };
    private VideoResolution HigherResolution(VideoResolution current)
    {
        if (_requestedResolution == VideoResolution.Auto || current == _requestedResolution) return current;
        return current switch
        {
            VideoResolution.Sd => VideoResolution.Hd,
            VideoResolution.Hd when _requestedResolution == VideoResolution.Fhd => VideoResolution.Fhd,
            _ => current
        };
    }
    private void ResetObservations()
    {
        _samples.Clear();
        _healthySince = null;
    }
}
