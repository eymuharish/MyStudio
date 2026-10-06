using TeleStudio.Contracts;
using TeleStudio.Settings;
using System.Text.Json;

namespace TeleStudio.Settings.Tests;

[CollectionDefinition("RoomDirectoryStore serial", DisableParallelization = true)]
public sealed class RoomDirectoryStoreSerialCollection;

[Collection("RoomDirectoryStore serial")]
public sealed class RoomDirectoryStoreTests : IDisposable
{
    private readonly string _directory = Path.Combine(
        Path.GetTempPath(), $"TeleStudio.RoomDirectoryStoreTests.{Guid.NewGuid():N}");
    private string PathUnderTest => Path.Combine(_directory, "rooms.json");
    private static RoomDirectoryEntry[] ExpectedSeed =>
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

    [Fact]
    public async Task Absent_file_returns_exact_seed_without_writing_and_each_snapshot_is_immutable()
    {
        Assert.Equal(
            ["Auto", "Sx20", "SonyXg77", "SonyXg80", "Generic", "Ex90"],
            Enum.GetNames<EndpointProfile>());
        var expected = ExpectedSeed;
        var store = new RoomDirectoryStore(PathUnderTest);

        var first = await store.LoadAsync(CancellationToken.None);
        var second = await store.LoadAsync(CancellationToken.None);

        Assert.Equal(expected, first);
        Assert.Equal(expected, second);
        Assert.NotSame(first, second);
        Assert.False(File.Exists(PathUnderTest));
        var list = Assert.IsAssignableFrom<IList<RoomDirectoryEntry>>(first);
        Assert.True(list.IsReadOnly);
        Assert.Throws<NotSupportedException>(() => list[0] = expected[1]);
    }

    [Fact]
    public async Task Save_then_load_preserves_add_edit_delete_stable_ids_and_profiles()
    {
        var store = new RoomDirectoryStore(PathUnderTest);
        await store.SaveAsync(
        [
            new("stable-a", "Alpha", "alpha.example", EndpointProfile.Auto),
            new("delete-me", "Delete", "delete.example", EndpointProfile.SonyXg80),
        ], CancellationToken.None);

        var expected = new RoomDirectoryEntry[]
        {
            new("stable-a", "Alpha edited", "2001:db8::10", EndpointProfile.SonyXg77),
            new("new-b", "Bravo", "192.0.2.20", EndpointProfile.SonyXg80),
        };
        await store.SaveAsync(expected, CancellationToken.None);

        Assert.Equal(expected, await store.LoadAsync(CancellationToken.None));
    }

    [Fact]
    public async Task Load_migrates_only_the_canonical_room_032_from_xg77_to_xg80_and_persists_it()
    {
        Directory.CreateDirectory(_directory);
        await File.WriteAllTextAsync(PathUnderTest,
            """
            [
              { "id": "room-032", "name": "استديو مخصص 032", "destination": "10.136.203.16", "endpointProfile": 2 },
              { "id": "custom-xg77", "name": "Custom", "destination": "10.136.203.99", "endpointProfile": 2 }
            ]
            """);

        var actual = await new RoomDirectoryStore(PathUnderTest).LoadAsync(CancellationToken.None);

        Assert.Equal(EndpointProfile.SonyXg80, actual.Single(entry => entry.Id == "room-032").EndpointProfile);
        Assert.Equal(EndpointProfile.SonyXg77, actual.Single(entry => entry.Id == "custom-xg77").EndpointProfile);

        using var persisted = JsonDocument.Parse(await File.ReadAllTextAsync(PathUnderTest));
        var persistedRoom032 = persisted.RootElement.EnumerateArray()
            .Single(entry => entry.GetProperty("id").GetString() == "room-032");
        Assert.Equal((int)EndpointProfile.SonyXg80, persistedRoom032.GetProperty("endpointProfile").GetInt32());
    }

    [Fact]
    public async Task Valid_empty_snapshot_round_trips_without_seeding()
    {
        var store = new RoomDirectoryStore(PathUnderTest);

        await store.SaveAsync([], CancellationToken.None);

        Assert.Empty(await store.LoadAsync(CancellationToken.None));
        Assert.Equal("[]", (await File.ReadAllTextAsync(PathUnderTest)).Trim());
    }

    [Fact]
    public async Task Save_trims_names_and_destinations_and_accepts_plain_ip_and_dns_hosts()
    {
        var store = new RoomDirectoryStore(PathUnderTest);
        var expected = new RoomDirectoryEntry[]
        {
            new("ipv4", "IPv4", "192.0.2.10", EndpointProfile.Generic),
            new("ipv6", "IPv6", "2001:db8::20", EndpointProfile.Sx20),
            new("dns", "قاعة عربية", "room.example.edu", EndpointProfile.SonyXg77),
        };

        await store.SaveAsync(
        [
            expected[0] with { Name = "  IPv4  ", Destination = "  192.0.2.10  " },
            expected[1],
            expected[2] with { Name = "  قاعة عربية ", Destination = " room.example.edu " },
        ], CancellationToken.None);

        Assert.Equal(expected, await store.LoadAsync(CancellationToken.None));
    }

    public static IEnumerable<object[]> InvalidSnapshots()
    {
        yield return [new RoomDirectoryEntry[] { new("id", " ", "host.example", EndpointProfile.Auto) }];
        yield return [new RoomDirectoryEntry[] { new("id", "Name", " ", EndpointProfile.Auto) }];
        yield return [new RoomDirectoryEntry[]
        {
            new("id-1", "Room", "one.example", EndpointProfile.Auto),
            new("id-2", "room", "two.example", EndpointProfile.Auto),
        }];
        yield return [new RoomDirectoryEntry[]
        {
            new("id-1", "One", "Host.Example", EndpointProfile.Auto),
            new("id-2", "Two", "host.example", EndpointProfile.Auto),
        }];
        yield return [new RoomDirectoryEntry[] { new(" ", "Name", "host.example", EndpointProfile.Auto) }];
        yield return [new RoomDirectoryEntry[]
        {
            new("same", "One", "one.example", EndpointProfile.Auto),
            new("same", "Two", "two.example", EndpointProfile.Auto),
        }];
        yield return [new RoomDirectoryEntry[] { new("id", "Name", "host.example", (EndpointProfile)999) }];
        yield return [new RoomDirectoryEntry[] { new("id", "Name", "https://host.example", EndpointProfile.Auto) }];
        yield return [new RoomDirectoryEntry[] { new("id", "Name", "host.example/path", EndpointProfile.Auto) }];
        yield return [new RoomDirectoryEntry[] { new("id", "Name", "host.example:5060", EndpointProfile.Auto) }];
        yield return [new RoomDirectoryEntry[] { new("id", "Name", "host name.example", EndpointProfile.Auto) }];
    }

    [Theory]
    [MemberData(nameof(InvalidSnapshots))]
    public async Task Invalid_snapshot_is_rejected_before_touching_filesystem(RoomDirectoryEntry[] entries)
    {
        var store = new RoomDirectoryStore(PathUnderTest);

        await Assert.ThrowsAsync<InvalidDataException>(
            () => store.SaveAsync(entries, CancellationToken.None));

        Assert.False(Directory.Exists(_directory));
    }

    [Fact]
    public async Task Search_matches_frozen_queries_and_sorts_seed_numerically()
    {
        var entries = await new RoomDirectoryStore(PathUnderTest).LoadAsync(CancellationToken.None);

        Assert.Equal(
            ["008", "018", "026", "032", "034", "042", "043", "046", "047"],
            RoomDirectoryStore.Search(entries, "  ").Select(entry => entry.Name));
        Assert.Equal(["047"], RoomDirectoryStore.Search(entries, "047").Select(entry => entry.Name));
        Assert.Equal(
            ["032", "042", "043"],
            RoomDirectoryStore.Search(entries, "10.136.203").Select(entry => entry.Name));
    }

    [Fact]
    public void Search_is_pure_case_insensitive_and_uses_exact_numeric_then_deterministic_ties()
    {
        var entries = new RoomDirectoryEntry[]
        {
            new("z", "2", "hall-two.example", EndpointProfile.Auto),
            new("b", "alpha", "alpha-b.example", EndpointProfile.Auto),
            new("a", "Alpha", "alpha-a.example", EndpointProfile.Auto),
            new("exact", "HALL", "exact.example", EndpointProfile.Auto),
            new("arabic", "قاعة الملك", "arabic.example", EndpointProfile.Auto),
        };
        var original = entries.ToArray();

        Assert.Equal(
            ["exact", "z"],
            RoomDirectoryStore.Search(entries, " hall ").Select(entry => entry.Id));
        Assert.Equal(
            ["arabic"],
            RoomDirectoryStore.Search(entries, "الملك").Select(entry => entry.Id));
        Assert.Equal(
            ["z", "a", "b", "exact", "arabic"],
            RoomDirectoryStore.Search(entries, null).Select(entry => entry.Id));
        Assert.Equal(original, entries);
        Assert.NotSame(entries, RoomDirectoryStore.Search(entries, null));
    }

    [Fact]
    public async Task First_save_and_existing_replacement_use_complete_json_and_remove_fixed_temp()
    {
        Directory.CreateDirectory(_directory);
        var temporaryPath = Path.Combine(_directory, "rooms.json.tmp");
        await File.WriteAllTextAsync(temporaryPath, "stale temporary data");
        var store = new RoomDirectoryStore(PathUnderTest);
        var first = new[] { new RoomDirectoryEntry("first", "First", "first.example", EndpointProfile.Auto) };
        var replacement = new[] { new RoomDirectoryEntry("second", "Second", "second.example", EndpointProfile.Generic) };

        await store.SaveAsync(first, CancellationToken.None);
        Assert.False(File.Exists(temporaryPath));
        await store.SaveAsync(replacement, CancellationToken.None);

        Assert.Equal(replacement, await store.LoadAsync(CancellationToken.None));
        Assert.False(File.Exists(temporaryPath));
        var json = await File.ReadAllTextAsync(PathUnderTest);
        Assert.Contains("\"id\": \"second\"", json, StringComparison.Ordinal);
        Assert.DoesNotContain("first", json, StringComparison.Ordinal);
    }

    public static IEnumerable<object[]> CorruptDocuments()
    {
        yield return ["{broken"];
        yield return [""];
        yield return ["{\"unsupported\":true}"];
        yield return ["[{\"id\":\"bad\",\"name\":\"\",\"destination\":\"host.example\",\"endpointProfile\":0}]"];
    }

    [Theory]
    [MemberData(nameof(CorruptDocuments))]
    public async Task Corrupt_document_moves_to_exact_utc_evidence_name_and_returns_seed(string document)
    {
        var instant = new DateTimeOffset(2026, 8, 31, 23, 59, 58, TimeSpan.Zero);
        Directory.CreateDirectory(_directory);
        await File.WriteAllTextAsync(PathUnderTest, document);
        RoomDirectoryStore.UtcNowForTests = () => instant;
        try
        {
            var actual = await new RoomDirectoryStore(PathUnderTest).LoadAsync(CancellationToken.None);

            Assert.Equal(ExpectedSeed, actual);
            Assert.False(File.Exists(PathUnderTest));
            var evidence = Path.Combine(_directory, "rooms.corrupt-20260831235958.json");
            Assert.True(File.Exists(evidence));
            Assert.Equal(document, await File.ReadAllTextAsync(evidence));
            Assert.False(File.Exists(Path.Combine(_directory, "rooms.json.tmp")));
        }
        finally
        {
            RoomDirectoryStore.UtcNowForTests = null;
        }
    }

    [Fact]
    public async Task Corrupt_backup_collision_fails_without_changing_original_or_evidence()
    {
        var instant = new DateTimeOffset(2026, 8, 31, 23, 59, 58, TimeSpan.Zero);
        Directory.CreateDirectory(_directory);
        await File.WriteAllTextAsync(PathUnderTest, "{broken");
        var evidence = Path.Combine(_directory, "rooms.corrupt-20260831235958.json");
        await File.WriteAllTextAsync(evidence, "existing evidence");
        RoomDirectoryStore.UtcNowForTests = () => instant;
        try
        {
            await Assert.ThrowsAsync<IOException>(
                () => new RoomDirectoryStore(PathUnderTest).LoadAsync(CancellationToken.None));

            Assert.Equal("{broken", await File.ReadAllTextAsync(PathUnderTest));
            Assert.Equal("existing evidence", await File.ReadAllTextAsync(evidence));
        }
        finally
        {
            RoomDirectoryStore.UtcNowForTests = null;
        }
    }

    [Fact]
    public async Task Valid_empty_json_does_not_create_corrupt_evidence()
    {
        Directory.CreateDirectory(_directory);
        await File.WriteAllTextAsync(PathUnderTest, "[]");

        var actual = await new RoomDirectoryStore(PathUnderTest).LoadAsync(CancellationToken.None);

        Assert.Empty(actual);
        Assert.True(File.Exists(PathUnderTest));
        Assert.Empty(Directory.GetFiles(_directory, "rooms.corrupt-*.json"));
    }

    [Fact]
    public async Task Held_load_then_replacement_save_are_linearized_across_store_instances()
    {
        var before = new[] { new RoomDirectoryEntry("before", "Before", "before.example", EndpointProfile.Auto) };
        var after = new[] { new RoomDirectoryEntry("after", "After", "after.example", EndpointProfile.Sx20) };
        var first = new RoomDirectoryStore(PathUnderTest);
        var samePath = new RoomDirectoryStore(Path.Combine(_directory, "nested", "..", "rooms.json"));
        await first.SaveAsync(before, CancellationToken.None);
        var entered = NewSignal();
        var release = NewSignal();
        RoomDirectoryStore.AfterGateAcquiredAsync = HoldFirst("load", entered, release);
        try
        {
            var load = first.LoadAsync(CancellationToken.None);
            await entered.Task.WaitAsync(TimeSpan.FromSeconds(5));
            var save = samePath.SaveAsync(after, CancellationToken.None);
            Assert.False(save.IsCompleted);

            release.SetResult();

            Assert.Equal(before, await load);
            await save;
            Assert.Equal(after, await first.LoadAsync(CancellationToken.None));
            Assert.False(File.Exists(Path.Combine(_directory, "rooms.json.tmp")));
        }
        finally
        {
            release.TrySetResult();
            RoomDirectoryStore.AfterGateAcquiredAsync = null;
        }
    }

    [Fact]
    public async Task Absent_load_then_first_save_returns_seed_before_complete_saved_state()
    {
        var saved = new[] { new RoomDirectoryEntry("saved", "Saved", "saved.example", EndpointProfile.Generic) };
        var first = new RoomDirectoryStore(PathUnderTest);
        var second = new RoomDirectoryStore(Path.Combine(_directory, ".", "rooms.json"));
        var entered = NewSignal();
        var release = NewSignal();
        RoomDirectoryStore.AfterGateAcquiredAsync = HoldFirst("load", entered, release);
        try
        {
            var load = first.LoadAsync(CancellationToken.None);
            await entered.Task.WaitAsync(TimeSpan.FromSeconds(5));
            var save = second.SaveAsync(saved, CancellationToken.None);
            Assert.False(save.IsCompleted);

            release.SetResult();

            Assert.Equal(ExpectedSeed, await load);
            await save;
            Assert.Equal(saved, await first.LoadAsync(CancellationToken.None));
            Assert.False(File.Exists(Path.Combine(_directory, "rooms.json.tmp")));
        }
        finally
        {
            release.TrySetResult();
            RoomDirectoryStore.AfterGateAcquiredAsync = null;
        }
    }

    [Fact]
    public async Task First_save_then_absent_load_returns_complete_saved_state()
    {
        var saved = new[] { new RoomDirectoryEntry("saved", "Saved", "saved.example", EndpointProfile.Generic) };
        var first = new RoomDirectoryStore(PathUnderTest);
        var second = new RoomDirectoryStore(Path.Combine(_directory, ".", "rooms.json"));
        var entered = NewSignal();
        var release = NewSignal();
        RoomDirectoryStore.AfterGateAcquiredAsync = HoldFirst("save", entered, release);
        try
        {
            var save = first.SaveAsync(saved, CancellationToken.None);
            await entered.Task.WaitAsync(TimeSpan.FromSeconds(5));
            var load = second.LoadAsync(CancellationToken.None);
            Assert.False(load.IsCompleted);

            release.SetResult();

            await save;
            Assert.Equal(saved, await load);
            Assert.False(File.Exists(Path.Combine(_directory, "rooms.json.tmp")));
        }
        finally
        {
            release.TrySetResult();
            RoomDirectoryStore.AfterGateAcquiredAsync = null;
        }
    }

    [Fact]
    public async Task Corrupt_recovery_then_save_preserves_one_evidence_file_and_complete_final_json()
    {
        Directory.CreateDirectory(_directory);
        await File.WriteAllTextAsync(PathUnderTest, "{broken");
        var saved = new[] { new RoomDirectoryEntry("saved", "Saved", "saved.example", EndpointProfile.Auto) };
        var first = new RoomDirectoryStore(PathUnderTest);
        var second = new RoomDirectoryStore(Path.Combine(_directory, ".", "rooms.json"));
        var entered = NewSignal();
        var release = NewSignal();
        RoomDirectoryStore.UtcNowForTests = () => new DateTimeOffset(2026, 9, 1, 0, 0, 1, TimeSpan.Zero);
        RoomDirectoryStore.AfterGateAcquiredAsync = HoldFirst("load", entered, release);
        try
        {
            var load = first.LoadAsync(CancellationToken.None);
            await entered.Task.WaitAsync(TimeSpan.FromSeconds(5));
            var save = second.SaveAsync(saved, CancellationToken.None);
            Assert.False(save.IsCompleted);

            release.SetResult();

            Assert.Equal(ExpectedSeed, await load);
            await save;
            Assert.Equal(saved, await first.LoadAsync(CancellationToken.None));
            Assert.Single(Directory.GetFiles(_directory, "rooms.corrupt-*.json"));
            Assert.True(File.Exists(Path.Combine(_directory, "rooms.corrupt-20260901000001.json")));
            Assert.False(File.Exists(Path.Combine(_directory, "rooms.json.tmp")));
        }
        finally
        {
            release.TrySetResult();
            RoomDirectoryStore.AfterGateAcquiredAsync = null;
            RoomDirectoryStore.UtcNowForTests = null;
        }
    }

    [Fact]
    public async Task Save_then_corrupt_load_sees_valid_saved_json_and_creates_no_evidence()
    {
        Directory.CreateDirectory(_directory);
        await File.WriteAllTextAsync(PathUnderTest, "{broken");
        var saved = new[] { new RoomDirectoryEntry("saved", "Saved", "saved.example", EndpointProfile.Auto) };
        var first = new RoomDirectoryStore(PathUnderTest);
        var second = new RoomDirectoryStore(Path.Combine(_directory, ".", "rooms.json"));
        var entered = NewSignal();
        var release = NewSignal();
        RoomDirectoryStore.AfterGateAcquiredAsync = HoldFirst("save", entered, release);
        try
        {
            var save = first.SaveAsync(saved, CancellationToken.None);
            await entered.Task.WaitAsync(TimeSpan.FromSeconds(5));
            var load = second.LoadAsync(CancellationToken.None);
            Assert.False(load.IsCompleted);

            release.SetResult();

            await save;
            Assert.Equal(saved, await load);
            Assert.Empty(Directory.GetFiles(_directory, "rooms.corrupt-*.json"));
            Assert.False(File.Exists(Path.Combine(_directory, "rooms.json.tmp")));
        }
        finally
        {
            release.TrySetResult();
            RoomDirectoryStore.AfterGateAcquiredAsync = null;
        }
    }

    [Fact]
    public async Task Two_same_path_saves_are_serialized_without_temp_or_partial_json()
    {
        var firstEntries = new[] { new RoomDirectoryEntry("first", "First", "first.example", EndpointProfile.Auto) };
        var secondEntries = new[] { new RoomDirectoryEntry("second", "Second", "second.example", EndpointProfile.SonyXg77) };
        var first = new RoomDirectoryStore(PathUnderTest);
        var second = new RoomDirectoryStore(Path.Combine(_directory, ".", "rooms.json"));
        var entered = NewSignal();
        var release = NewSignal();
        RoomDirectoryStore.AfterGateAcquiredAsync = HoldFirst("save", entered, release);
        try
        {
            var firstSave = first.SaveAsync(firstEntries, CancellationToken.None);
            await entered.Task.WaitAsync(TimeSpan.FromSeconds(5));
            var secondSave = second.SaveAsync(secondEntries, CancellationToken.None);
            Assert.False(secondSave.IsCompleted);

            release.SetResult();

            await Task.WhenAll(firstSave, secondSave);
            Assert.Equal(secondEntries, await first.LoadAsync(CancellationToken.None));
            Assert.False(File.Exists(Path.Combine(_directory, "rooms.json.tmp")));
        }
        finally
        {
            release.TrySetResult();
            RoomDirectoryStore.AfterGateAcquiredAsync = null;
        }
    }

    private static TaskCompletionSource NewSignal() =>
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    private static Func<string, string, CancellationToken, Task> HoldFirst(
        string operation,
        TaskCompletionSource entered,
        TaskCompletionSource release)
    {
        var held = 0;
        return async (actualOperation, _, cancellationToken) =>
        {
            if (actualOperation != operation || Interlocked.Exchange(ref held, 1) != 0)
            {
                return;
            }

            entered.SetResult();
            await release.Task.WaitAsync(cancellationToken);
        };
    }

    public void Dispose()
    {
        RoomDirectoryStore.AfterGateAcquiredAsync = null;
        RoomDirectoryStore.UtcNowForTests = null;
        if (Directory.Exists(_directory))
        {
            Directory.Delete(_directory, recursive: true);
        }
    }
}
