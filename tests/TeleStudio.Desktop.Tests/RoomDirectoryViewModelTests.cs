using TeleStudio.Contracts;
using System.Diagnostics;
using System.Resources;

namespace TeleStudio.Desktop.Tests;

public sealed class RoomDirectoryViewModelTests
{
    [Theory]
    [InlineData("047", 1)]
    [InlineData("10.136.203", 3)]
    [InlineData("قاعة المحاضرات", 1)]
    public void Search_projects_room_name_or_address(string query, int count)
    {
        var entries = Seed().ToArray();
        entries[1] = entries[1] with { Name = "قاعة المحاضرات" };
        var vm = new RoomDirectoryViewModel(entries, _ => Task.CompletedTask);

        vm.SearchText = query;

        Assert.Equal(count, vm.VisibleEntries.Count);
    }

    [Fact]
    public void Selection_is_stable_by_id_and_never_dials()
    {
        var vm = new RoomDirectoryViewModel(Seed(), _ => Task.CompletedTask);
        vm.Select("046");
        vm.SearchText = "10.136.51";

        Assert.Equal("046", vm.SelectedEntryId);
        Assert.Equal("10.136.51.23", vm.EditorDestination);
        Assert.Equal(EndpointProfile.Sx20, vm.EditorEndpoint);
    }

    [Fact]
    public void Add_edit_and_delete_require_valid_unique_values_and_confirmation()
    {
        var vm = new RoomDirectoryViewModel(Seed(), _ => Task.CompletedTask);
        vm.BeginAdd();
        vm.EditorName = "046";
        vm.EditorDestination = "10.136.51.99";
        vm.EditorEndpoint = EndpointProfile.Sx20;
        Assert.False(vm.TryApplyEdit());
        Assert.Contains("unique", vm.ValidationMessage, StringComparison.OrdinalIgnoreCase);

        vm.EditorName = "قاعة جديدة";
        vm.EditorDestination = "10.136.51.99";
        Assert.True(vm.TryApplyEdit());
        var id = vm.SelectedEntryId!;
        Assert.False(vm.DeleteSelected(false));
        Assert.Contains(vm.WorkingEntries, entry => entry.Id == id);
        Assert.True(vm.DeleteSelected(true));
        Assert.DoesNotContain(vm.WorkingEntries, entry => entry.Id == id);
    }

    [Fact]
    public async Task Save_publishes_live_snapshot_only_after_store_success_and_cancel_is_inert()
    {
        var saveCount = 0;
        IReadOnlyList<RoomDirectoryItem>? published = null;
        var vm = new RoomDirectoryViewModel(Seed(), value => { saveCount++; return Task.CompletedTask; }, snapshot => published = snapshot);
        vm.BeginAdd();
        vm.EditorName = "مختبر";
        vm.EditorDestination = "10.136.70.10";
        vm.EditorEndpoint = EndpointProfile.Generic;
        Assert.True(vm.TryApplyEdit());

        await vm.SaveAsync(default);

        Assert.Equal(1, saveCount);
        Assert.NotNull(published);
        Assert.Equal(10, published!.Count);

        var cancelled = new RoomDirectoryViewModel(Seed(), value => { saveCount++; return Task.CompletedTask; }, snapshot => published = snapshot);
        cancelled.BeginAdd();
        cancelled.EditorName = "لن يحفظ";
        cancelled.EditorDestination = "10.136.70.11";
        cancelled.Cancel();
        Assert.Equal(1, saveCount);
    }

    [Fact]
    public async Task Failed_save_keeps_editor_and_reports_non_destructive_error()
    {
        var vm = new RoomDirectoryViewModel(Seed(), _ => throw new IOException("simulated"));
        vm.BeginAdd();
        vm.EditorName = "قاعة جديدة";
        vm.EditorDestination = "10.136.70.10";
        vm.EditorEndpoint = EndpointProfile.Generic;
        Assert.True(vm.TryApplyEdit());

        Assert.False(await vm.SaveAsync(default));

        Assert.Equal("قاعة جديدة", vm.SelectedEntry!.Name);
        Assert.Contains("not saved", vm.ValidationMessage, StringComparison.OrdinalIgnoreCase);
    }

    private static IReadOnlyList<RoomDirectoryItem> Seed() =>
    [
        new("008", "008", "10.136.51.22", EndpointProfile.Sx20),
        new("026", "026", "10.136.101.11", EndpointProfile.SonyXg77),
        new("032", "032", "10.136.203.16", EndpointProfile.SonyXg80),
        new("034", "034", "10.136.51.11", EndpointProfile.SonyXg77),
        new("042", "042", "10.136.203.13", EndpointProfile.SonyXg77),
        new("043", "043", "10.136.203.14", EndpointProfile.SonyXg77),
        new("046", "046", "10.136.51.23", EndpointProfile.Sx20),
        new("047", "047", "10.136.51.25", EndpointProfile.Sx20),
        new("018", "018", "10.136.61.31", EndpointProfile.SonyXg77),
    ];

}
