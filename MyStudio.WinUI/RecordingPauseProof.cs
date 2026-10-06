namespace MyStudio.WinUI;

internal sealed record RecordingPauseProof(double MinimumActiveMs, double MaximumActiveMs, double ExcludedPauseWallMs)
{
    internal static RecordingPauseProof Measure(double started, double pauseRequested, double paused, double resumeRequested,
        double resumed, double stopRequested, double stopped)
    {
        var observations = new[] { 0, started, pauseRequested, paused, resumeRequested, resumed, stopRequested, stopped };
        if (observations.Any(x => !double.IsFinite(x)) || observations.Zip(observations.Skip(1)).Any(x => x.First > x.Second))
            throw new ArgumentException("Recording observations must be monotonic.");
        // Each native transition occurs between sending its command and observing its acknowledgment.
        return new((pauseRequested - started) + (stopRequested - resumed),
            paused + (stopped - resumeRequested), resumeRequested - paused);
    }
    internal bool AcceptsElapsed(long elapsedMs) => elapsedMs >= Math.Max(0, MinimumActiveMs - 100)
        && elapsedMs <= MaximumActiveMs + 100; // Allow one media/timer reporting interval at the endpoints.
}
