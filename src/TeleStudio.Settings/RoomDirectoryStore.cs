using System.Collections.Concurrent;
using System.Net;
using System.Globalization;
using System.Numerics;
using System.Runtime.CompilerServices;
using System.Text.Json;
using TeleStudio.Contracts;

[assembly: InternalsVisibleTo("TeleStudio.Settings.Tests")]

namespace TeleStudio.Settings;

public sealed class RoomDirectoryStore : IRoomDirectoryStore
{
    private static readonly ConcurrentDictionary<string, SemaphoreSlim> Gates =
        new(StringComparer.OrdinalIgnoreCase);

    internal static Func<string, string, CancellationToken, Task>? AfterGateAcquiredAsync { get; set; }
    internal static Func<DateTimeOffset>? UtcNowForTests { get; set; }

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        WriteIndented = true,
    };

    private static readonly RoomDirectoryEntry[] Seed =
    [
        new("room-008", "008", "10.136.51.22", EndpointProfile.Sx20),
        new("room-026", "026", "10.136.101.11", EndpointProfile.SonyXg77),
        new("room-032", "032", "10.136.203.16", EndpointProfile.SonyXg80),
        new("room-034", "034", "10.136.51.11", EndpointProfile.SonyXg77),
        new("room-042", "042", "10.136.203.13", EndpointProfile.SonyXg77),
        new("room-043", "043", "10.136.203.14", EndpointProfile.SonyXg77),
        new("room-046", "046", "10.136.51.23", EndpointProfile.Sx20),
        new("room-047", "047", "10.136.51.25", EndpointProfile.Sx20),
        new("room-018", "018", "10.136.61.31", EndpointProfile.SonyXg77),
    ];

    private readonly string _path;

    public RoomDirectoryStore()
        : this(AppDataPaths.OverrideRoot is { } isolated ? Path.Combine(isolated, "rooms.json") : Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "TeleStudio",
            "rooms.json"))
    {
    }

    public RoomDirectoryStore(string path)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        _path = Path.GetFullPath(path);
    }

    public async Task<IReadOnlyList<RoomDirectoryEntry>> LoadAsync(CancellationToken cancellationToken)
    {
        var gate = Gates.GetOrAdd(_path, static _ => new SemaphoreSlim(1, 1));
        await gate.WaitAsync(cancellationToken);
        try
        {
            await InvokeAfterGateAsync("load", cancellationToken);
            if (!File.Exists(_path))
            {
                return Snapshot(Seed);
            }

            try
            {
                RoomDirectoryEntry[] entries;
                await using (var stream = new FileStream(
                    _path,
                    FileMode.Open,
                    FileAccess.Read,
                    FileShare.None,
                    bufferSize: 16 * 1024,
                    FileOptions.Asynchronous | FileOptions.SequentialScan))
                {
                    entries = await JsonSerializer.DeserializeAsync<RoomDirectoryEntry[]>(
                        stream,
                        JsonOptions,
                        cancellationToken) ?? throw new JsonException("Room directory document is empty");
                }
                var normalized = ValidateAndNormalize(entries);
                var migrated = MigrateKnownSonyProfiles(normalized, out var changed);
                if (changed)
                {
                    await WriteNormalizedUnderGateAsync(migrated, cancellationToken);
                }
                return migrated;
            }
            catch (Exception error) when (
                error is JsonException or NotSupportedException or InvalidDataException)
            {
                PreserveCorruptFile();
                return Snapshot(Seed);
            }
        }
        finally
        {
            gate.Release();
        }
    }

    public async Task SaveAsync(
        IReadOnlyList<RoomDirectoryEntry> entries,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(entries);
        var normalized = ValidateAndNormalize(entries);
        var gate = Gates.GetOrAdd(_path, static _ => new SemaphoreSlim(1, 1));
        await gate.WaitAsync(cancellationToken);

        try
        {
            await InvokeAfterGateAsync("save", cancellationToken);
            await WriteNormalizedUnderGateAsync(normalized, cancellationToken);
        }
        finally
        {
            gate.Release();
        }
    }

    public static IReadOnlyList<RoomDirectoryEntry> Search(
        IReadOnlyList<RoomDirectoryEntry> entries,
        string? query)
    {
        ArgumentNullException.ThrowIfNull(entries);
        var normalizedQuery = query?.Trim() ?? string.Empty;
        var candidates = entries
            .Where(entry => entry is not null &&
                (normalizedQuery.Length == 0 ||
                 entry.Name.Contains(normalizedQuery, StringComparison.OrdinalIgnoreCase) ||
                 entry.Destination.Contains(normalizedQuery, StringComparison.OrdinalIgnoreCase)))
            .Select(entry =>
            {
                var numeric = BigInteger.TryParse(
                    entry.Name,
                    NumberStyles.None,
                    CultureInfo.InvariantCulture,
                    out var numericName);
                return new SearchCandidate(
                    entry,
                    normalizedQuery.Length != 0 && string.Equals(
                        entry.Name,
                        normalizedQuery,
                        StringComparison.OrdinalIgnoreCase),
                    numeric,
                    numericName);
            })
            .OrderByDescending(candidate => candidate.ExactName)
            .ThenBy(candidate => !candidate.Numeric)
            .ThenBy(candidate => candidate.NumericName)
            .ThenBy(candidate => candidate.Entry.Name, StringComparer.OrdinalIgnoreCase)
            .ThenBy(candidate => candidate.Entry.Id, StringComparer.Ordinal)
            .Select(candidate => candidate.Entry);
        return Snapshot(candidates);
    }

    private static IReadOnlyList<RoomDirectoryEntry> ValidateAndNormalize(
        IReadOnlyList<RoomDirectoryEntry> entries)
    {
        var ids = new HashSet<string>(StringComparer.Ordinal);
        var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var destinations = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var normalized = new RoomDirectoryEntry[entries.Count];

        for (var index = 0; index < entries.Count; index++)
        {
            var entry = entries[index];
            if (entry is null || string.IsNullOrWhiteSpace(entry.Id) ||
                !Enum.IsDefined(entry.EndpointProfile))
            {
                throw new InvalidDataException("Room directory entry has an invalid identity or profile");
            }

            var name = entry.Name?.Trim();
            var destination = entry.Destination?.Trim();
            if (string.IsNullOrEmpty(name) || string.IsNullOrEmpty(destination) ||
                destination.Any(char.IsWhiteSpace) || !IsDirectDestination(destination))
            {
                throw new InvalidDataException("Room directory entry has an invalid name or destination");
            }
            if (!ids.Add(entry.Id) || !names.Add(name) || !destinations.Add(destination))
            {
                throw new InvalidDataException("Room directory IDs, names, and destinations must be unique");
            }

            normalized[index] = entry with { Name = name, Destination = destination };
        }

        return Snapshot(normalized);
    }

    private static IReadOnlyList<RoomDirectoryEntry> MigrateKnownSonyProfiles(
        IReadOnlyList<RoomDirectoryEntry> entries,
        out bool changed)
    {
        changed = false;
        var migrated = entries.ToArray();
        for (var index = 0; index < migrated.Length; index++)
        {
            var entry = migrated[index];
            if (entry.Id == "room-032" &&
                entry.Destination == "10.136.203.16" &&
                entry.EndpointProfile == EndpointProfile.SonyXg77)
            {
                migrated[index] = entry with { EndpointProfile = EndpointProfile.SonyXg80 };
                changed = true;
            }
        }

        return changed ? Snapshot(migrated) : entries;
    }

    private async Task WriteNormalizedUnderGateAsync(
        IReadOnlyList<RoomDirectoryEntry> normalized,
        CancellationToken cancellationToken)
    {
        var directory = Path.GetDirectoryName(_path)
            ?? throw new InvalidOperationException("Room directory path requires a parent directory");
        var temporaryPath = Path.Combine(directory, $"{Path.GetFileName(_path)}.tmp");
        Directory.CreateDirectory(directory);
        try
        {
            await using (var stream = new FileStream(
                temporaryPath,
                FileMode.Create,
                FileAccess.Write,
                FileShare.None,
                bufferSize: 16 * 1024,
                FileOptions.Asynchronous | FileOptions.WriteThrough))
            {
                await JsonSerializer.SerializeAsync(stream, normalized, JsonOptions, cancellationToken);
                await stream.FlushAsync(cancellationToken);
                stream.Flush(flushToDisk: true);
            }

            if (File.Exists(_path))
            {
                File.Replace(temporaryPath, _path, destinationBackupFileName: null);
            }
            else
            {
                File.Move(temporaryPath, _path);
            }
        }
        finally
        {
            if (File.Exists(temporaryPath))
            {
                File.Delete(temporaryPath);
            }
        }
    }

    private static bool IsDirectDestination(string destination) =>
        IPAddress.TryParse(destination, out _) || Uri.CheckHostName(destination) == UriHostNameType.Dns;

    private void PreserveCorruptFile()
    {
        var directory = Path.GetDirectoryName(_path)
            ?? throw new InvalidOperationException("Room directory path requires a parent directory");
        var timestamp = (UtcNowForTests?.Invoke() ?? DateTimeOffset.UtcNow).ToString(
            "yyyyMMddHHmmss",
            CultureInfo.InvariantCulture);
        var backupPath = Path.Combine(
            directory,
            $"{Path.GetFileNameWithoutExtension(_path)}.corrupt-{timestamp}.json");
        File.Move(_path, backupPath);
    }

    private Task InvokeAfterGateAsync(string operation, CancellationToken cancellationToken) =>
        AfterGateAcquiredAsync?.Invoke(operation, _path, cancellationToken) ?? Task.CompletedTask;

    private readonly record struct SearchCandidate(
        RoomDirectoryEntry Entry,
        bool ExactName,
        bool Numeric,
        BigInteger NumericName);

    private static IReadOnlyList<RoomDirectoryEntry> Snapshot(
        IEnumerable<RoomDirectoryEntry> entries) => Array.AsReadOnly(entries.ToArray());
}
