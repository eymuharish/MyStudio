using TeleStudio.Contracts;

namespace TeleStudio.Presentation;

public interface IEngineCommandSink
{
    Task SendAsync(EngineCommand command, CancellationToken cancellationToken);
}
