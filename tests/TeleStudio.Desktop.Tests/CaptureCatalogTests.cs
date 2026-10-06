using System.Runtime.InteropServices;
using System.Text;
using TeleStudio.Desktop;

namespace TeleStudio.Desktop.Tests;

public sealed class CaptureCatalogTests
{
    [Fact]
    public async Task Refresh_decodes_exact_offsets_and_returns_an_immutable_ordered_snapshot_on_one_mta_thread()
    {
        var native = FakeNative.Valid([
            (CaptureItemKind.Monitor, "monitor-primary", "Primary display", 1920, 1080),
            (CaptureItemKind.Monitor, "monitor-z", "Display Z", 1280, 720),
            (CaptureItemKind.Window, "window-a", "محاضرة", 1024, 768),
        ]);
        await using var catalog = new NativeCaptureCatalog(native);

        var first = await catalog.RefreshAsync(CancellationToken.None);
        var second = await catalog.RefreshAsync(CancellationToken.None);

        Assert.True(first.IsAvailable, first.ErrorCode);
        Assert.Equal(3, first.Items.Count);
        Assert.Equal("محاضرة", first.Items[2].Name);
        Assert.IsAssignableFrom<IReadOnlyList<CaptureCatalogItem>>(first.Items);
        Assert.Single(native.ThreadIds);
        Assert.All(native.Apartments, state => Assert.Equal(ApartmentState.MTA, state));
        Assert.NotSame(first.Items, second.Items);
    }

    [Fact]
    public async Task Catalog_changed_restarts_from_sizing_at_most_three_times_then_is_typed_unavailable()
    {
        var native = FakeNative.AlwaysChanged();
        await using var catalog = new NativeCaptureCatalog(native);

        var result = await catalog.RefreshAsync(CancellationToken.None);

        Assert.False(result.IsAvailable);
        Assert.Equal("capture-catalog-unavailable", result.ErrorCode);
        Assert.Equal(3, native.SizingCalls);
        Assert.Equal(3, native.FillCalls);
    }

    [Theory]
    [InlineData(FakeFault.OverlappingRanges)]
    [InlineData(FakeFault.DuplicateId)]
    [InlineData(FakeFault.InvalidDimensions)]
    [InlineData(FakeFault.InvalidUtf8)]
    [InlineData(FakeFault.WrongStructSize)]
    [InlineData(FakeFault.WrongOrder)]
    public async Task Malformed_native_output_fails_closed_without_exposing_identifiers(FakeFault fault)
    {
        var native = FakeNative.Malformed(fault);
        await using var catalog = new NativeCaptureCatalog(native);

        var result = await catalog.RefreshAsync(CancellationToken.None);

        Assert.False(result.IsAvailable);
        Assert.Equal("capture-catalog-invalid", result.ErrorCode);
        Assert.DoesNotContain("synthetic-secret", result.ErrorCode, StringComparison.Ordinal);
    }

    [Fact]
    public async Task Cancellation_suppresses_publication_but_does_not_abort_native_query()
    {
        var native = FakeNative.Valid([(CaptureItemKind.Monitor, "monitor-one", "Monitor", 1920, 1080)]);
        native.BlockFill = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        using var cancellation = new CancellationTokenSource();
        await using var catalog = new NativeCaptureCatalog(native);

        var refresh = catalog.RefreshAsync(cancellation.Token);
        await native.FillEntered.Task;
        cancellation.Cancel();
        native.BlockFill.TrySetResult();

        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => refresh);
        await catalog.DisposeAsync();
        Assert.Equal(1, native.CompletedFillCalls);
    }

    [Fact]
    public async Task Blocked_query_disposal_returns_bounded_without_unloading_in_use_native_api()
    {
        var native = FakeNative.Valid([(CaptureItemKind.Monitor, "monitor-one", "Monitor", 1920, 1080)]);
        native.BlockFill = new(TaskCreationOptions.RunContinuationsAsynchronously);
        var catalog = new NativeCaptureCatalog(native);
        var refresh = catalog.RefreshAsync(default);
        await native.FillEntered.Task.WaitAsync(TimeSpan.FromSeconds(2));
        var disposal = catalog.DisposeAsync().AsTask();
        try
        {
            await disposal.WaitAsync(TimeSpan.FromSeconds(3));
            Assert.False(native.NativeDisposed.Task.IsCompleted);
            var retired = await refresh.WaitAsync(TimeSpan.FromSeconds(1));
            Assert.False(retired.IsAvailable);
            Assert.Equal("capture-catalog-disposed", retired.ErrorCode);
            await Assert.ThrowsAsync<ObjectDisposedException>(() => catalog.RefreshAsync(default));
        }
        finally
        {
            native.BlockFill.TrySetResult();
            await disposal;
            await native.NativeDisposed.Task.WaitAsync(TimeSpan.FromSeconds(2));
        }
        Assert.False(native.DisposedWhileQueryActive);
        Assert.Equal(1, native.DisposeCount);
    }

    [Fact]
    public async Task Blocked_refresh_returns_typed_unavailable_before_native_query_finishes()
    {
        var native = FakeNative.Valid([(CaptureItemKind.Monitor, "monitor-one", "Monitor", 1920, 1080)]);
        native.BlockFill = new(TaskCreationOptions.RunContinuationsAsynchronously);
        var catalog = new NativeCaptureCatalog(native);
        try
        {
            var result = await catalog.RefreshAsync(default).WaitAsync(TimeSpan.FromSeconds(6));
            Assert.False(result.IsAvailable);
            Assert.Equal("capture-catalog-timeout", result.ErrorCode);
            Assert.False(native.NativeDisposed.Task.IsCompleted);
        }
        finally { native.BlockFill.TrySetResult(); await catalog.DisposeAsync(); }
        Assert.False(native.DisposedWhileQueryActive);
    }

    [Fact]
    public async Task Blocked_query_retries_are_bounded_and_one_cancelled_caller_does_not_cancel_another()
    {
        var native = FakeNative.Valid([(CaptureItemKind.Monitor, "monitor-one", "Monitor", 1920, 1080)]);
        native.BlockFill = new(TaskCreationOptions.RunContinuationsAsynchronously);
        await using var catalog = new NativeCaptureCatalog(native);
        using var cancellation = new CancellationTokenSource();
        var first = catalog.RefreshAsync(cancellation.Token);
        await native.FillEntered.Task.WaitAsync(TimeSpan.FromSeconds(2));
        var second = catalog.RefreshAsync(default);
        try
        {
            cancellation.Cancel();
            await Assert.ThrowsAnyAsync<OperationCanceledException>(() => first);
            for (var i = 0; i < 100; ++i)
            {
                var retry = await catalog.RefreshAsync(default).WaitAsync(TimeSpan.FromSeconds(1));
                Assert.False(retry.IsAvailable);
                Assert.Equal("capture-catalog-busy", retry.ErrorCode);
            }
            Assert.False(second.IsCompleted);
            Assert.Equal(1, native.FillCalls);
            native.BlockFill.TrySetResult();
            Assert.True((await second.WaitAsync(TimeSpan.FromSeconds(2))).IsAvailable);
        }
        finally { native.BlockFill.TrySetResult(); }
    }

    [Fact]
    public async Task Wrong_abi_is_typed_unavailable_and_native_layout_is_frozen()
    {
        var native = FakeNative.Valid([]);
        native.AbiVersion = 2;
        await using var catalog = new NativeCaptureCatalog(native);

        var result = await catalog.RefreshAsync(CancellationToken.None);

        Assert.False(result.IsAvailable);
        Assert.Equal(32, Marshal.SizeOf<NativeCaptureCatalogItem>());
    }

    public enum FakeFault { None, OverlappingRanges, DuplicateId, InvalidDimensions, InvalidUtf8, WrongStructSize, WrongOrder }

    private sealed class FakeNative : ICaptureCatalogNativeApi, IDisposable
    {
        private readonly List<(CaptureItemKind Kind, string Id, string Name, int Width, int Height)> _items;
        private readonly bool _alwaysChanged;
        private readonly FakeFault _fault;
        public uint AbiVersion { get; set; } = 1;
        public int SizingCalls { get; private set; }
        public int FillCalls { get; private set; }
        public int CompletedFillCalls { get; private set; }
        public HashSet<int> ThreadIds { get; } = [];
        public List<ApartmentState> Apartments { get; } = [];
        public TaskCompletionSource FillEntered { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource? BlockFill { get; set; }
        public TaskCompletionSource NativeDisposed { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public bool DisposedWhileQueryActive { get; private set; }
        public int DisposeCount { get; private set; }
        public void Dispose()
        {
            DisposeCount++;
            DisposedWhileQueryActive = BlockFill is not null && !BlockFill.Task.IsCompleted;
            NativeDisposed.TrySetResult();
        }

        private FakeNative(List<(CaptureItemKind, string, string, int, int)> items, bool alwaysChanged = false, FakeFault fault = FakeFault.None)
        {
            _items = items;
            _alwaysChanged = alwaysChanged;
            _fault = fault;
        }

        public static FakeNative Valid(List<(CaptureItemKind, string, string, int, int)> items) => new(items);
        public static FakeNative AlwaysChanged() => new([(CaptureItemKind.Monitor, "monitor-one", "Monitor", 1, 1)], true);
        public static FakeNative Malformed(FakeFault fault)
        {
            var items = fault == FakeFault.WrongOrder
                ? new List<(CaptureItemKind, string, string, int, int)> { (CaptureItemKind.Window, "window-one", "Window", 10, 10), (CaptureItemKind.Monitor, "monitor-one", "Monitor", 10, 10) }
                : [(CaptureItemKind.Monitor, "synthetic-secret", "Synthetic secret", 10, 10), (CaptureItemKind.Window, "window-two", "Window", 10, 10)];
            return new FakeNative(items, fault: fault);
        }

        uint ICaptureCatalogNativeApi.GetAbiVersion() => AbiVersion;

        public CaptureCatalogStatus Query(uint abiVersion, ulong expectedSnapshot, Span<NativeCaptureCatalogItem> items,
            Span<byte> utf8, out uint requiredItems, out uint requiredUtf8, out ulong snapshot)
        {
            ThreadIds.Add(Environment.CurrentManagedThreadId);
            Apartments.Add(Thread.CurrentThread.GetApartmentState());
            var encoded = _items.Select(item => (Id: Encoding.UTF8.GetBytes(item.Id), Name: Encoding.UTF8.GetBytes(item.Name))).ToArray();
            requiredItems = (uint)_items.Count;
            requiredUtf8 = (uint)encoded.Sum(item => item.Id.Length + item.Name.Length);
            snapshot = 42;
            if (items.IsEmpty && utf8.IsEmpty)
            {
                SizingCalls++;
                return _items.Count == 0 ? CaptureCatalogStatus.Ok : CaptureCatalogStatus.BufferTooSmall;
            }

            FillCalls++;
            FillEntered.TrySetResult();
            BlockFill?.Task.GetAwaiter().GetResult();
            if (_alwaysChanged)
            {
                return CaptureCatalogStatus.CatalogChanged;
            }
            var offset = 0;
            for (var index = 0; index < _items.Count; index++)
            {
                var item = _items[index];
                var id = encoded[index].Id;
                var name = encoded[index].Name;
                id.CopyTo(utf8[offset..]);
                var idOffset = offset;
                offset += id.Length;
                name.CopyTo(utf8[offset..]);
                var nameOffset = offset;
                offset += name.Length;
                items[index] = new NativeCaptureCatalogItem
                {
                    StructSize = _fault == FakeFault.WrongStructSize ? 31u : 32u,
                    Kind = (uint)item.Kind + (_fault == FakeFault.WrongOrder ? 0u : 0u),
                    Width = (uint)(_fault == FakeFault.InvalidDimensions && index == 0 ? 0 : item.Width),
                    Height = (uint)item.Height,
                    IdOffset = (uint)(_fault == FakeFault.OverlappingRanges && index == 1 ? 0 : idOffset),
                    IdLength = (uint)id.Length,
                    NameOffset = (uint)nameOffset,
                    NameLength = (uint)name.Length,
                };
            }
            if (_fault == FakeFault.DuplicateId && items.Length > 1)
            {
                items[1].IdOffset = items[0].IdOffset;
                items[1].IdLength = items[0].IdLength;
            }
            if (_fault == FakeFault.InvalidUtf8 && utf8.Length > 0)
            {
                utf8[0] = 0xff;
            }
            CompletedFillCalls++;
            return CaptureCatalogStatus.Ok;
        }
    }
}
