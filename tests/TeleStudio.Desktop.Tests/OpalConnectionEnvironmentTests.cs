using TeleStudio.Contracts;
using TeleStudio.Presentation;
using TeleStudio.Settings;

namespace TeleStudio.Desktop.Tests;

public sealed class OpalConnectionEnvironmentTests
{
    [Theory]
    [InlineData(NetworkConnectionMode.Building)]
    [InlineData(NetworkConnectionMode.ExternalVpn)]
    public async Task Global_mode_applies_at_startup_and_every_profile_protocol_switch_despite_conflicting_profile_values(NetworkConnectionMode mode)
    {
        var root = Directory.CreateTempSubdirectory("OPAL environment ");
        try
        {
            var path = Path.Combine(root.FullName, "connection-environment.json");
            var store = new OpalConnectionModeStore(path);
            await store.SaveAsync(mode, default);
            foreach (var endpoint in new[] { EndpointProfile.Sx20, EndpointProfile.Ex90, EndpointProfile.SonyXg77, EndpointProfile.SonyXg80 })
            {
                // A new controller represents startup or a family reinitialization.
                var environment = new OpalConnectionEnvironment(new OpalConnectionModeStore(path));
                await environment.LoadAsync(default);
                var engine = new Sink(); var calls = new MainViewModel(engine) { SelectedEndpoint = endpoint };
                foreach (var protocol in Enum.GetValues<CallProtocol>())
                {
                    calls.SelectedProtocol = protocol;
                    calls.SelectedNetworkMode = mode == NetworkConnectionMode.Building ? NetworkConnectionMode.ExternalVpn : NetworkConnectionMode.Building;
                    await environment.ApplyAsync(engine, calls, default);
                    Assert.Equal(NetworkConnectionMode.ExternalVpn, calls.SelectedNetworkMode);
                    Assert.Equal(new SetNetworkMode(NetworkConnectionMode.ExternalVpn), engine.Commands.Last());
                }
            }
            Assert.Single(Directory.GetFiles(root.FullName));
        }
        finally { root.Delete(true); }
    }

    [Fact]
    public async Task Idle_change_persists_globally_and_active_call_change_cannot_write_or_send()
    {
        var root = Directory.CreateTempSubdirectory("OPAL environment lifecycle ");
        try
        {
            var path = Path.Combine(root.FullName, "connection-environment.json");
            var environment = new OpalConnectionEnvironment(new OpalConnectionModeStore(path));
            await environment.LoadAsync(default);
            var engine = new Sink(); var calls = new MainViewModel(engine);
            await environment.ChangeAsync(NetworkConnectionMode.ExternalVpn, true, engine, calls, default);
            Assert.Equal(NetworkConnectionMode.ExternalVpn, await new OpalConnectionModeStore(path).LoadAsync(default));
            calls.ApplyEvent(new CallConnected("10.0.0.20", CallProtocol.Sip, 1920, 1080, 30));
            var commands = engine.Commands.Count;
            await Assert.ThrowsAsync<InvalidOperationException>(() => environment.ChangeAsync(NetworkConnectionMode.Building, true, engine, calls, default));
            Assert.Equal(commands, engine.Commands.Count);
            Assert.Equal(NetworkConnectionMode.ExternalVpn, await new OpalConnectionModeStore(path).LoadAsync(default));
        }
        finally { root.Delete(true); }
    }

    private sealed class Sink : IEngineCommandSink
    {
        public List<EngineCommand> Commands { get; } = [];
        public Task SendAsync(EngineCommand command, CancellationToken token) { Commands.Add(command); return Task.CompletedTask; }
    }
}
