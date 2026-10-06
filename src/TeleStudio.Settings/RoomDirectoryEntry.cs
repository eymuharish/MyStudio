using TeleStudio.Contracts;

namespace TeleStudio.Settings;

public sealed record RoomDirectoryEntry(
    string Id,
    string Name,
    string Destination,
    EndpointProfile EndpointProfile);

public interface IRoomDirectoryStore
{
    Task<IReadOnlyList<RoomDirectoryEntry>> LoadAsync(CancellationToken cancellationToken);

    Task SaveAsync(
        IReadOnlyList<RoomDirectoryEntry> entries,
        CancellationToken cancellationToken);
}
