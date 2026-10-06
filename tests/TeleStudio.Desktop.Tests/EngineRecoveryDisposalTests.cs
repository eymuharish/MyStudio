using TeleStudio.Contracts;
namespace TeleStudio.Desktop.Tests;
public sealed class EngineRecoveryDisposalTests
{
    [Fact] public async Task FailureOfFirstConsoleStillDisposesSecond()
    {
        var first = new Console(CallProtocol.H323, true); var second = new Console(CallProtocol.Sip, false);
        var sink = new NativeEngineCommandSink(first, second);
        await Assert.ThrowsAnyAsync<Exception>(async () => await sink.DisposeAsync());
        Assert.True(second.Disposed);
    }
    private sealed class Console(CallProtocol protocol, bool fail) : IProtocolConsole
    {
        public bool Disposed;
        public CallProtocol Protocol => protocol;
        public long ProcessGeneration => 1;
        public event EventHandler<ProtocolConsoleLine>? LineReceived { add {} remove {} }
        public event EventHandler<ProtocolConsoleGenerationEnded>? GenerationEnded { add {} remove {} }
        public Task StartAsync(CancellationToken token) => Task.CompletedTask;
        public Task WriteLineAsync(string command, CancellationToken token) => Task.CompletedTask;
        public ValueTask DisposeAsync() { Disposed = true; if(fail) throw new IOException("shutdown failed"); return ValueTask.CompletedTask; }
    }
}
