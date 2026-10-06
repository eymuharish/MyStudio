using TeleStudio.Contracts;
using TeleStudio.Presentation;

namespace TeleStudio.Desktop;

public sealed class DesktopDemoEngineCommandSink : IEngineCommandSink
{
    private readonly TimeSpan _latency;
    private string _remoteAddress = "studio-endpoint";
    private CallProtocol _protocol = CallProtocol.H323;
    private int _dialGeneration;

    public DesktopDemoEngineCommandSink(TimeSpan? latency = null) => _latency = latency ?? TimeSpan.FromMilliseconds(350);

    public event EventHandler<EngineEvent>? EventReceived;

    public async Task SendAsync(EngineCommand command, CancellationToken cancellationToken)
    {
        switch (command)
        {
            case Dial dial:
                var generation = Interlocked.Increment(ref _dialGeneration);
                _remoteAddress = dial.Destination;
                _protocol = dial.Protocol;
                await DelayAsync(cancellationToken);
                if (generation == Volatile.Read(ref _dialGeneration))
                {
                    Raise(new CallConnected(_remoteAddress, _protocol, 1920, 1080, 30));
                }
                break;
            case Answer:
                await DelayAsync(cancellationToken);
                Raise(new CallConnected(_remoteAddress, _protocol, 1920, 1080, 30));
                break;
            case Reject:
                Raise(new CallEnded("declined"));
                break;
            case SwitchSource source:
                await DelayAsync(cancellationToken);
                Raise(new LocalSourceChanged(source.Source));
                break;
            case SetMuted muted:
                Raise(new MuteChanged(muted.IsMuted));
                break;
            case HangUp:
                Interlocked.Increment(ref _dialGeneration);
                Raise(new CallEnding());
                await DelayAsync(cancellationToken);
                Raise(new CallEnded("local-hangup"));
                break;
        }
    }

    private Task DelayAsync(CancellationToken cancellationToken) => _latency == TimeSpan.Zero
        ? Task.CompletedTask
        : Task.Delay(_latency, cancellationToken);

    private void Raise(EngineEvent engineEvent) => EventReceived?.Invoke(this, engineEvent);
}
