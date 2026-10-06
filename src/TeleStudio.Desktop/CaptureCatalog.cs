using System.Collections.Concurrent;
using System.Collections.ObjectModel;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace TeleStudio.Desktop;

public enum CaptureItemKind { Monitor = 1, Window = 2, Camera = 3 }

public sealed record CaptureCatalogItem(string Id, CaptureItemKind Kind, string Name, int Width, int Height);

public sealed record CaptureCatalogSnapshot(bool IsAvailable, IReadOnlyList<CaptureCatalogItem> Items, string? ErrorCode)
{
    internal static CaptureCatalogSnapshot Available(IReadOnlyList<CaptureCatalogItem> items) => new(true, items, null);
    internal static CaptureCatalogSnapshot Unavailable(string code) => new(false, Array.Empty<CaptureCatalogItem>(), code);
}

public interface ICaptureCatalog
{
    Task<CaptureCatalogSnapshot> RefreshAsync(CancellationToken cancellationToken);
}

internal enum CaptureCatalogStatus
{
    Ok = 0,
    BufferTooSmall = 1,
    CatalogChanged = 2,
    AbiMismatch = 3,
    InvalidArgument = 4,
    NotSupported = 5,
    EnumerationFailed = 6,
    LimitExceeded = 7,
    WrongApartment = 8,
    InternalFailure = 9
}

[StructLayout(LayoutKind.Sequential, Pack = 4)]
internal struct NativeCaptureCatalogItem
{
    public uint StructSize;
    public uint Kind;
    public uint Width;
    public uint Height;
    public uint IdOffset;
    public uint IdLength;
    public uint NameOffset;
    public uint NameLength;
}

internal interface ICaptureCatalogNativeApi
{
    uint GetAbiVersion();
    CaptureCatalogStatus Query(
        uint abiVersion,
        ulong expectedSnapshot,
        Span<NativeCaptureCatalogItem> items,
        Span<byte> utf8,
        out uint requiredItems,
        out uint requiredUtf8,
        out ulong snapshot);
}

public sealed class NativeCaptureCatalog : ICaptureCatalog, IAsyncDisposable
{
    private const int MaximumItems = 1024;
    private const int MaximumBlobBytes = 4 * 1024 * 1024;
    private readonly ICaptureCatalogNativeApi _native;
    private readonly bool _cameras;
    private readonly BlockingCollection<RefreshRequest> _requests = new();
    private readonly Thread _worker;
    private readonly object _lifecycle = new();
    private readonly HashSet<TaskCompletionSource<CaptureCatalogSnapshot>> _pending = [];
    private readonly TaskCompletionSource _exited = new(TaskCreationOptions.RunContinuationsAsynchronously);
    private bool _disposed;

    public NativeCaptureCatalog() : this(new PInvokeCaptureCatalogNativeApi())
    {
    }

    public NativeCaptureCatalog(bool cameras) : this(new PInvokeCaptureCatalogNativeApi(cameras), cameras) { }
    public NativeCaptureCatalog(string libraryDirectory, bool cameras = false) : this(new PInvokeCaptureCatalogNativeApi(cameras, libraryDirectory), cameras) { }

    internal NativeCaptureCatalog(ICaptureCatalogNativeApi native, bool cameras = false)
    {
        _cameras = cameras;
        _native = native ?? throw new ArgumentNullException(nameof(native));
        _worker = new Thread(WorkerMain)
        {
            IsBackground = true,
            Name = "EssaStudio capture catalog MTA"
        };
        _worker.SetApartmentState(ApartmentState.MTA);
        _worker.Start();
    }

    public async Task<CaptureCatalogSnapshot> RefreshAsync(CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var completion = new TaskCompletionSource<CaptureCatalogSnapshot>(TaskCreationOptions.RunContinuationsAsynchronously);
        lock (_lifecycle)
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            if (_pending.Count >= 2) return CaptureCatalogSnapshot.Unavailable("capture-catalog-busy");
            _pending.Add(completion);
            _requests.Add(new RefreshRequest(completion));
        }
        try { return await completion.Task.WaitAsync(TimeSpan.FromSeconds(5), cancellationToken); }
        catch (TimeoutException)
        {
            var unavailable = CaptureCatalogSnapshot.Unavailable("capture-catalog-timeout");
            completion.TrySetResult(unavailable);
            return unavailable;
        }
        catch (OperationCanceledException) { completion.TrySetCanceled(cancellationToken); throw; }
    }

    public async ValueTask DisposeAsync()
    {
        lock (_lifecycle)
        {
            if (!_disposed)
            {
                _disposed = true;
                _requests.CompleteAdding();
                foreach (var request in _pending)
                    request.TrySetResult(CaptureCatalogSnapshot.Unavailable("capture-catalog-disposed"));
            }
        }
        // Native enumeration cannot be interrupted safely. The worker retains the DLL
        // until the query returns; closing the UI does not wait indefinitely.
        try { await _exited.Task.WaitAsync(TimeSpan.FromSeconds(2)); }
        catch (TimeoutException) { }
    }

    private void WorkerMain()
    {
        try
        {
            foreach (var request in _requests.GetConsumingEnumerable())
            {
                try
                {
                    if (!request.Completion.Task.IsCompleted)
                        request.Completion.TrySetResult(ExecuteRefresh());
                }
                catch (Exception error) when (IsNativeAvailabilityFailure(error))
                {
                    request.Completion.TrySetResult(CaptureCatalogSnapshot.Unavailable("capture-catalog-unavailable"));
                }
                catch
                {
                    request.Completion.TrySetResult(CaptureCatalogSnapshot.Unavailable("capture-catalog-invalid"));
                }
                finally { lock (_lifecycle) _pending.Remove(request.Completion); }
            }
        }
        finally
        {
            try { (_native as IDisposable)?.Dispose(); _requests.Dispose(); _exited.TrySetResult(); }
            catch (Exception error) { _exited.TrySetException(error); }
        }
    }

    private CaptureCatalogSnapshot ExecuteRefresh()
    {
        if (_native.GetAbiVersion() != 1)
        {
            return CaptureCatalogSnapshot.Unavailable("capture-catalog-unavailable");
        }

        for (var attempt = 0; attempt < 3; attempt++)
        {
            var sizingStatus = _native.Query(1, 0, Span<NativeCaptureCatalogItem>.Empty, Span<byte>.Empty,
                out var requiredItems, out var requiredUtf8, out var snapshot);
            if (snapshot == 0 || requiredItems > MaximumItems || requiredUtf8 > MaximumBlobBytes)
            {
                return CaptureCatalogSnapshot.Unavailable("capture-catalog-invalid");
            }
            if (requiredItems == 0)
            {
                return sizingStatus == CaptureCatalogStatus.Ok && requiredUtf8 == 0
                    ? CaptureCatalogSnapshot.Available(Array.Empty<CaptureCatalogItem>())
                    : CaptureCatalogSnapshot.Unavailable("capture-catalog-invalid");
            }
            if (sizingStatus != CaptureCatalogStatus.BufferTooSmall || requiredUtf8 == 0)
            {
                return CaptureCatalogSnapshot.Unavailable("capture-catalog-unavailable");
            }

            var nativeItems = new NativeCaptureCatalogItem[requiredItems];
            var utf8 = new byte[requiredUtf8];
            var fillStatus = _native.Query(1, snapshot, nativeItems, utf8,
                out var filledItems, out var filledUtf8, out var filledSnapshot);
            if (fillStatus is CaptureCatalogStatus.BufferTooSmall or CaptureCatalogStatus.CatalogChanged)
            {
                continue;
            }
            if (fillStatus != CaptureCatalogStatus.Ok)
            {
                return CaptureCatalogSnapshot.Unavailable("capture-catalog-unavailable");
            }
            if (filledItems != requiredItems || filledUtf8 != requiredUtf8 || filledSnapshot != snapshot || filledSnapshot == 0)
            {
                return CaptureCatalogSnapshot.Unavailable("capture-catalog-invalid");
            }
            return ValidateAndProject(nativeItems, utf8);
        }
        return CaptureCatalogSnapshot.Unavailable("capture-catalog-unavailable");
    }

    private CaptureCatalogSnapshot ValidateAndProject(
        IReadOnlyList<NativeCaptureCatalogItem> nativeItems,
        byte[] utf8)
    {
        var decoder = new UTF8Encoding(false, true);
        var items = new CaptureCatalogItem[nativeItems.Count];
        var ranges = new List<(int Start, int End)>(nativeItems.Count * 2);
        var identifiers = new HashSet<string>(StringComparer.Ordinal);
        for (var index = 0; index < nativeItems.Count; index++)
        {
            var native = nativeItems[index];
            if (native.StructSize != 32 || (_cameras ? native.Kind != 3 || native.Width != 0 || native.Height != 0 :
                native.Kind is not (1 or 2) || native.Width is < 1 or > 16384 || native.Height is < 1 or > 16384) ||
                native.IdLength < 1 || native.IdLength > (_cameras ? 2048 : 1024) || native.NameLength is < 1 or > 2048 ||
                !TryRange(native.IdOffset, native.IdLength, utf8.Length, out var idRange) ||
                !TryRange(native.NameOffset, native.NameLength, utf8.Length, out var nameRange))
            {
                return CaptureCatalogSnapshot.Unavailable("capture-catalog-invalid");
            }
            ranges.Add(idRange);
            ranges.Add(nameRange);
            string id;
            string name;
            try
            {
                id = decoder.GetString(utf8.AsSpan(idRange.Start, idRange.End - idRange.Start));
                name = decoder.GetString(utf8.AsSpan(nameRange.Start, nameRange.End - nameRange.Start));
            }
            catch (DecoderFallbackException)
            {
                return CaptureCatalogSnapshot.Unavailable("capture-catalog-invalid");
            }
            if (id.Any(char.IsControl) || name.Contains('\0') || CountScalars(name) is < 1 or > 512 || !identifiers.Add(id))
            {
                return CaptureCatalogSnapshot.Unavailable("capture-catalog-invalid");
            }
            items[index] = new CaptureCatalogItem(id, (CaptureItemKind)native.Kind, name, (int)native.Width, (int)native.Height);
        }

        ranges.Sort(static (left, right) => left.Start.CompareTo(right.Start));
        for (var index = 1; index < ranges.Count; index++)
        {
            if (ranges[index].Start < ranges[index - 1].End)
            {
                return CaptureCatalogSnapshot.Unavailable("capture-catalog-invalid");
            }
        }
        if (!_cameras && !HasValidOrder(items))
        {
            return CaptureCatalogSnapshot.Unavailable("capture-catalog-invalid");
        }
        return CaptureCatalogSnapshot.Available(new ReadOnlyCollection<CaptureCatalogItem>(items));
    }

    private static bool HasValidOrder(IReadOnlyList<CaptureCatalogItem> items)
    {
        var windowStarted = false;
        string? priorMonitorId = null;
        CaptureCatalogItem? priorWindow = null;
        for (var index = 0; index < items.Count; index++)
        {
            var item = items[index];
            if (item.Kind == CaptureItemKind.Monitor)
            {
                if (windowStarted)
                {
                    return false;
                }
                if (index > 1 && priorMonitorId is not null && StringComparer.Ordinal.Compare(priorMonitorId, item.Id) > 0)
                {
                    return false;
                }
                priorMonitorId = item.Id;
            }
            else
            {
                windowStarted = true;
                if (priorWindow is not null)
                {
                    var nameOrder = StringComparer.OrdinalIgnoreCase.Compare(priorWindow.Name, item.Name);
                    if (nameOrder > 0 || nameOrder == 0 && StringComparer.Ordinal.Compare(priorWindow.Id, item.Id) > 0)
                    {
                        return false;
                    }
                }
                priorWindow = item;
            }
        }
        return true;
    }

    private static bool TryRange(uint offset, uint length, int bufferLength, out (int Start, int End) range)
    {
        var end = (ulong)offset + length;
        if (end > (ulong)bufferLength)
        {
            range = default;
            return false;
        }
        range = ((int)offset, (int)end);
        return true;
    }

    private static int CountScalars(string value)
    {
        var count = 0;
        foreach (var _ in value.EnumerateRunes()) count++;
        return count;
    }

    private static bool IsNativeAvailabilityFailure(Exception error) => error is
        DllNotFoundException or EntryPointNotFoundException or BadImageFormatException or TypeInitializationException;

    private sealed record RefreshRequest(
        TaskCompletionSource<CaptureCatalogSnapshot> Completion);
}

internal sealed partial class PInvokeCaptureCatalogNativeApi(bool cameras = false, string? libraryDirectory = null) : ICaptureCatalogNativeApi, IDisposable
{
    private IntPtr _library;
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate uint AbiFunction();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int QueryFunction(uint abi, ulong expected, IntPtr items, uint count, IntPtr utf8, uint bytes, out uint requiredItems, out uint requiredBytes, out ulong snapshot);
    private T Function<T>(string name) where T : Delegate
    {
        if (_library == IntPtr.Zero) _library = NativeLibrary.Load(System.IO.Path.Combine(libraryDirectory!, "EssaStudio.CaptureCatalog.dll"));
        return Marshal.GetDelegateForFunctionPointer<T>(NativeLibrary.GetExport(_library, name));
    }
    public uint GetAbiVersion() => libraryDirectory is null ? EssaCaptureCatalogAbiVersion() : Function<AbiFunction>("EssaCaptureCatalogAbiVersion")();
    public void Dispose() { if (_library != IntPtr.Zero) { NativeLibrary.Free(_library); _library = IntPtr.Zero; } }

    public CaptureCatalogStatus Query(uint abiVersion, ulong expectedSnapshot, Span<NativeCaptureCatalogItem> items,
        Span<byte> utf8, out uint requiredItems, out uint requiredUtf8, out ulong snapshot)
    {
        var itemBytes = checked(items.Length * Marshal.SizeOf<NativeCaptureCatalogItem>());
        var itemBuffer = itemBytes == 0 ? IntPtr.Zero : Marshal.AllocHGlobal(itemBytes);
        var utf8Buffer = utf8.IsEmpty ? IntPtr.Zero : Marshal.AllocHGlobal(utf8.Length);
        try
        {
            var status = libraryDirectory is not null ? Function<QueryFunction>(cameras ? "EssaCameraCatalogQueryV1" : "EssaCaptureCatalogQueryV1")(abiVersion, expectedSnapshot, itemBuffer, (uint)items.Length,
                utf8Buffer, (uint)utf8.Length, out requiredItems, out requiredUtf8, out snapshot) : cameras ? EssaCameraCatalogQueryV1(abiVersion, expectedSnapshot, itemBuffer, (uint)items.Length,
                utf8Buffer, (uint)utf8.Length, out requiredItems, out requiredUtf8, out snapshot) :
                EssaCaptureCatalogQueryV1(abiVersion, expectedSnapshot, itemBuffer, (uint)items.Length,
                utf8Buffer, (uint)utf8.Length, out requiredItems, out requiredUtf8, out snapshot);
            if (status == (int)CaptureCatalogStatus.Ok)
            {
                if (itemBytes != 0)
                {
                    var bytes = new byte[itemBytes];
                    Marshal.Copy(itemBuffer, bytes, 0, bytes.Length);
                    MemoryMarshal.Cast<byte, NativeCaptureCatalogItem>(bytes).CopyTo(items);
                }
                if (!utf8.IsEmpty)
                {
                    var bytes = new byte[utf8.Length];
                    Marshal.Copy(utf8Buffer, bytes, 0, bytes.Length);
                    bytes.CopyTo(utf8);
                }
            }
            return (CaptureCatalogStatus)status;
        }
        finally
        {
            if (itemBuffer != IntPtr.Zero) Marshal.FreeHGlobal(itemBuffer);
            if (utf8Buffer != IntPtr.Zero) Marshal.FreeHGlobal(utf8Buffer);
        }
    }

    [LibraryImport("EssaStudio.CaptureCatalog.dll", EntryPoint = "EssaCaptureCatalogAbiVersion")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    private static partial uint EssaCaptureCatalogAbiVersion();

    [LibraryImport("EssaStudio.CaptureCatalog.dll", EntryPoint = "EssaCaptureCatalogQueryV1")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    private static partial int EssaCaptureCatalogQueryV1(
        uint abiVersion, ulong expectedSnapshot, IntPtr items, uint itemCapacity,
        IntPtr utf8, uint utf8Capacity, out uint requiredItems, out uint requiredUtf8, out ulong snapshot);

    [LibraryImport("EssaStudio.CaptureCatalog.dll", EntryPoint = "EssaCameraCatalogQueryV1")]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    private static partial int EssaCameraCatalogQueryV1(
        uint abiVersion, ulong expectedSnapshot, IntPtr items, uint itemCapacity,
        IntPtr utf8, uint utf8Capacity, out uint requiredItems, out uint requiredUtf8, out ulong snapshot);
}
