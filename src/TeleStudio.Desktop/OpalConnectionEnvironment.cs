using TeleStudio.Contracts;
using TeleStudio.Presentation;
using TeleStudio.Settings;

namespace TeleStudio.Desktop;

public sealed class OpalConnectionEnvironment(OpalConnectionModeStore store)
{
    public NetworkConnectionMode Mode { get; private set; } = NetworkConnectionMode.ExternalVpn;
    public async Task LoadAsync(CancellationToken token) => Mode = await store.LoadAsync(token);

    public async Task ApplyAsync(IEngineCommandSink engine, MainViewModel calls, CancellationToken token)
    {
        await engine.SendAsync(new SetNetworkMode(Mode), token);
        calls.SelectedNetworkMode = Mode;
    }

    public async Task ChangeAsync(NetworkConnectionMode mode, bool idle, IEngineCommandSink engine, MainViewModel calls, CancellationToken token)
    {
        if (!idle || calls.CanHangUp) throw new InvalidOperationException("End the call before changing the connection environment.");
        if (!Enum.IsDefined(mode)) throw new ArgumentOutOfRangeException(nameof(mode));
        mode = NetworkConnectionMode.ExternalVpn;
        await engine.SendAsync(new SetNetworkMode(mode), token);
        try { await store.SaveAsync(mode, token); }
        catch
        {
            await engine.SendAsync(new SetNetworkMode(Mode), CancellationToken.None);
            throw;
        }
        Mode = mode; calls.SelectedNetworkMode = mode;
    }
}
