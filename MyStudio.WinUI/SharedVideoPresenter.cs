using System.IO;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.WindowsRuntime;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media.Imaging;
using TeleStudio.Contracts;
using TeleStudio.Desktop;

namespace MyStudio.WinUI;

/// <summary>Real shared-D3D11 frames copied into WinUI pixels. This measured CPU path is explicit.</summary>
public sealed class SharedVideoPresenter : IDisposable
{
    private readonly Image image;
    private readonly SemaphoreSlim operations = new(1, 1);
    private SharedFrameReader? reader;
    private Func<SurfaceAcknowledgement, ulong, CancellationToken, Task>? acknowledge;
    private WriteableBitmap? bitmap;
    private ulong epoch;
    public long PresentedFrames { get; private set; }
    public double LastCopyMilliseconds { get; private set; }
    public ulong LastFingerprint { get; private set; }
    public string? Error { get; private set; }
    public string Mode => "D3D11 shared surface → CPU BGRA → WinUI WriteableBitmap";
    public SharedVideoPresenter(Image image) => this.image = image;
    public async Task AttachAsync(RemoteSurfaceChanged publication,
        Func<SurfaceAcknowledgement, ulong, CancellationToken, Task> acknowledgement, CancellationToken token)
    {
        await operations.WaitAsync(token);
        try
        {
            if (publication.Epoch <= epoch) { CloseHandle((nint)publication.HostHandleValue); return; }
            await RetireAsync(false, token);
            epoch = publication.Epoch; acknowledge = acknowledgement;
            try
            {
                reader = new SharedFrameReader(publication);
                bitmap = new WriteableBitmap(publication.VideoWidth, publication.VideoHeight);
                image.Source = bitmap; Error = null;
                await acknowledgement(SurfaceAcknowledgement.Opened, epoch, token);
            }
            catch (Exception error)
            {
                Error = error.Message; reader?.Dispose(); reader = null;
                await acknowledgement(SurfaceAcknowledgement.Removed, epoch, token);
                epoch = 0; throw;
            }
        }
        finally { operations.Release(); }
    }
    public bool Present()
    {
        if (reader is null || bitmap is null || operations.CurrentCount == 0) return false;
        var clock = Stopwatch.StartNew();
        try
        {
            if (!reader.TryRead(out var pixels)) return false;
            using var stream = bitmap.PixelBuffer.AsStream(); stream.Position = 0; stream.Write(pixels);
            bitmap.Invalidate(); PresentedFrames++; LastCopyMilliseconds = clock.Elapsed.TotalMilliseconds;
            ulong hash = 1469598103934665603;
            for (var offset = 0; offset < pixels.Length; offset += 4096) hash = (hash ^ pixels[offset]) * 1099511628211;
            LastFingerprint = hash; return true;
        }
        catch (Exception error) { Error = error.Message; return false; }
    }
    public async Task DetachAsync(bool terminal, CancellationToken token)
    {
        await operations.WaitAsync(token);
        try { await RetireAsync(terminal, token); } finally { operations.Release(); }
    }
    private async Task RetireAsync(bool terminal, CancellationToken token)
    {
        var prior = epoch; epoch = 0; reader?.Dispose(); reader = null; bitmap = null; image.Source = null;
        if (prior != 0 && acknowledge is { } callback)
            try { await callback(terminal ? SurfaceAcknowledgement.Removed : SurfaceAcknowledgement.Released, prior, token); } catch { }
        acknowledge = null;
    }
    public void Dispose() { reader?.Dispose(); reader = null; image.Source = null; }
    [DllImport("kernel32")] private static extern bool CloseHandle(nint handle);

    private sealed unsafe class SharedFrameReader : IDisposable
    {
        private nint device, device1, context, texture, mutex, staging, handle;
        private readonly byte[] pixels;
        private readonly int width, height;
        public SharedFrameReader(RemoteSurfaceChanged publication)
        {
            width = publication.VideoWidth; height = publication.VideoHeight;
            if (width is < 1 or > 8192 || height is < 1 or > 8192 || publication.HostHandleValue == 0)
                throw new ArgumentException("Invalid native video publication");
            handle = (nint)publication.HostHandleValue; pixels = new byte[checked(width * height * 4)];
            try
            {
                var hr = D3D11CreateDevice(0, 1, 0, 0x20, 0, 0, 7, out device, out _, out context);
                if (hr < 0) hr = D3D11CreateDevice(0, 5, 0, 0x20, 0, 0, 7, out device, out _, out context);
                Marshal.ThrowExceptionForHR(hr);
                var id = new Guid("a04bfb29-08ef-43d6-a49c-a9bdbdcbe686");
                Marshal.ThrowExceptionForHR(Marshal.QueryInterface(device, in id, out device1));
                id = new Guid("6f15aaf2-d208-4e89-9ab4-489535d34f9c");
                var open = (delegate* unmanaged[Stdcall]<nint, nint, Guid*, nint*, int>)(*(nint**)device1)[48];
                nint resource; Marshal.ThrowExceptionForHR(open(device1, handle, &id, &resource)); texture = resource;
                id = new Guid("9d8e1289-d7b3-465f-8126-250e349af85d");
                Marshal.ThrowExceptionForHR(Marshal.QueryInterface(texture, in id, out mutex));
                TextureDescription desc;
                ((delegate* unmanaged[Stdcall]<nint, TextureDescription*, void>)(*(nint**)texture)[10])(texture, &desc);
                if (desc.Width != width || desc.Height != height || desc.Format != 87) throw new InvalidOperationException("Native texture format differs from BGRA publication");
                desc.Usage = 3; desc.BindFlags = 0; desc.CpuAccess = 0x20000; desc.MiscFlags = 0;
                var create = (delegate* unmanaged[Stdcall]<nint, TextureDescription*, nint, nint*, int>)(*(nint**)device)[5];
                Marshal.ThrowExceptionForHR(create(device, &desc, 0, &resource)); staging = resource;
            }
            catch { Dispose(); throw; }
        }
        public bool TryRead(out byte[] frame)
        {
            frame = pixels;
            var acquire = (delegate* unmanaged[Stdcall]<nint, ulong, uint, int>)(*(nint**)mutex)[8];
            if (acquire(mutex, 1, 0) != 0) return false;
            try { ((delegate* unmanaged[Stdcall]<nint, nint, nint, void>)(*(nint**)context)[47])(context, staging, texture); }
            finally { ((delegate* unmanaged[Stdcall]<nint, ulong, int>)(*(nint**)mutex)[9])(mutex, 0); }
            var map = (delegate* unmanaged[Stdcall]<nint, nint, uint, uint, uint, MappedResource*, int>)(*(nint**)context)[14];
            MappedResource mapped;
            var result = map(context, staging, 0, 1, 0, &mapped); Marshal.ThrowExceptionForHR(result);
            try { for (var row = 0; row < height; row++) Marshal.Copy(mapped.Data + checked((int)(row * mapped.RowPitch)), pixels, row * width * 4, width * 4); }
            finally { ((delegate* unmanaged[Stdcall]<nint, nint, uint, void>)(*(nint**)context)[15])(context, staging, 0); }
            return true;
        }
        public void Dispose()
        {
            foreach (var pointer in new[] { staging, mutex, texture, context, device1, device }) if (pointer != 0) Marshal.Release(pointer);
            staging = mutex = texture = context = device1 = device = 0;
            if (handle != 0) CloseHandle(handle); handle = 0;
        }
        [StructLayout(LayoutKind.Sequential)] private struct TextureDescription
        { public uint Width, Height, MipLevels, ArraySize, Format, SampleCount, SampleQuality, Usage, BindFlags, CpuAccess, MiscFlags; }
        [StructLayout(LayoutKind.Sequential)] private struct MappedResource { public nint Data; public uint RowPitch, DepthPitch; }
        [DllImport("d3d11")] private static extern int D3D11CreateDevice(nint adapter, uint type, nint software, uint flags, nint levels, uint count, uint sdk, out nint device, out uint feature, out nint context);
    }
}
