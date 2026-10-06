using System.Text;
namespace TeleStudio.Desktop.Tests;
public class CameraCatalogTests
{
    private sealed class Api : ICaptureCatalogNativeApi
    {
        public uint GetAbiVersion() => 1;
        public CaptureCatalogStatus Query(uint version, ulong expected, Span<NativeCaptureCatalogItem> items, Span<byte> utf8,
            out uint count, out uint bytes, out ulong snapshot)
        {
            var data = Encoding.UTF8.GetBytes("stable-idFriendly camera"); count=1; bytes=(uint)data.Length; snapshot=1;
            if (items.IsEmpty) return CaptureCatalogStatus.BufferTooSmall;
            items[0] = new() { StructSize=32, Kind=3, IdLength=9, NameOffset=9, NameLength=15 };
            data.CopyTo(utf8); return CaptureCatalogStatus.Ok;
        }
    }
    [Fact]
    public async Task Camera_query_accepts_unknown_dimensions_and_preserves_real_identifier_and_friendly_name()
    {
        await using var catalog = new NativeCaptureCatalog(new Api(), cameras: true);
        var result = await catalog.RefreshAsync(default);
        Assert.True(result.IsAvailable); var item=Assert.Single(result.Items);
        Assert.Equal("stable-id",item.Id); Assert.Equal("Friendly camera",item.Name); Assert.Equal(0,item.Width);
    }
    [Fact]
    public async Task Existing_desktop_query_rejects_camera_records()
    {
        await using var catalog = new NativeCaptureCatalog(new Api());
        Assert.False((await catalog.RefreshAsync(default)).IsAvailable);
    }
}
