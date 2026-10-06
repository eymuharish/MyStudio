using System.Text;
using TeleStudio.Contracts;

namespace TeleStudio.Desktop;

public sealed record ProtocolConsoleLine(ReadOnlyMemory<byte> Bytes, bool IsError, long ProcessGeneration)
{
    private static readonly UTF8Encoding StrictUtf8 = new(false, true);

    public ProtocolConsoleLine(string text, bool isError, long processGeneration = 1)
        : this(StrictUtf8.GetBytes(text.EndsWith('\n') ? text : text + "\n"), isError, processGeneration)
    {
    }

    public bool IsJsonShaped
    {
        get
        {
            var span = Bytes.Span;
            var index = 0;
            if (span.StartsWith(new byte[] { 0xef, 0xbb, 0xbf }))
            {
                index = 3;
            }
            while (index < span.Length && span[index] is (byte)' ' or (byte)'\t' or (byte)'\r' or (byte)'\n')
            {
                index++;
            }
            return index < span.Length && span[index] == (byte)'{';
        }
    }

    public bool TryGetLegacyText(out string text)
    {
        try
        {
            var span = Bytes.Span;
            if (span.Length > 0 && span[^1] == (byte)'\n')
            {
                span = span[..^1];
            }
            text = StrictUtf8.GetString(span).TrimStart('>', ' ');
            return true;
        }
        catch (DecoderFallbackException)
        {
            text = string.Empty;
            return false;
        }
    }
}

public sealed record ProtocolConsoleGenerationEnded(long ProcessGeneration);

public interface IProtocolConsole : IAsyncDisposable
{
    CallProtocol Protocol { get; }
    long ProcessGeneration { get; }
    event EventHandler<ProtocolConsoleLine>? LineReceived;
    event EventHandler<ProtocolConsoleGenerationEnded>? GenerationEnded;
    void ConfigureVideoStartup(SetVideoPreferences preferences) { }
    Task StartAsync(CancellationToken cancellationToken);
    Task PrepareDialAsync(string destination, CancellationToken cancellationToken) => Task.CompletedTask;
    Task AbortDialAsync(CancellationToken cancellationToken) => WriteLineAsync("hangup", cancellationToken);
    Task WriteLineAsync(string command, CancellationToken cancellationToken);
}
