namespace TeleStudio.Desktop;

internal sealed class ApplicationShutdownCoordinator(TimeSpan hangupTimeout)
{
    private int _state;

    public bool IsRunning => Volatile.Read(ref _state) == 1;
    public bool FinalCloseAllowed => Volatile.Read(ref _state) == 2;

    public async Task<bool> ExecuteAsync(
        bool confirmed,
        bool hasActiveCall,
        Func<CancellationToken, Task> hangUp,
        Func<ValueTask> cleanup)
    {
        if (!confirmed) return false;
        if (Interlocked.CompareExchange(ref _state, 1, 0) != 0) return false;

        var elapsed = System.Diagnostics.Stopwatch.StartNew();
        var totalTimeout = TimeSpan.FromSeconds(5);
        if (hasActiveCall)
        {
            try
            {
                var allowed = hangupTimeout < totalTimeout ? hangupTimeout : totalTimeout;
                using var timeout = new CancellationTokenSource(allowed);
                await hangUp(timeout.Token).WaitAsync(allowed);
            }
            catch (Exception exception) when (exception is not OutOfMemoryException)
            {
                // Disposal below sends quit and force-terminates an engine that
                // cannot acknowledge hangup within the bounded exit window.
            }
        }

        try
        {
            var remaining = totalTimeout - elapsed.Elapsed;
            var cleanupTask = cleanup().AsTask();
            await cleanupTask.WaitAsync(remaining > TimeSpan.Zero ? remaining : TimeSpan.FromMilliseconds(1));
        }
        catch (Exception exception) when (exception is not OutOfMemoryException) { }
        finally { Runtime.OwnedEngineProcesses.TerminateChildren(); }
        Volatile.Write(ref _state, 2);
        return true;
    }
}
