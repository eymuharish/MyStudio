using System.Runtime.InteropServices;
using TeleStudio.Desktop;

namespace MyStudio.WinUI;
public sealed partial class MainWindow
{
    private bool mainCaptureExcluded;
    private bool ApplySharingCaptureExclusion(CaptureItemKind? kind)
    {
        // A full monitor must not capture its own live preview recursively.
        // Explicit camera/window sharing keeps normal window-capture behavior.
        var exclude = kind == CaptureItemKind.Monitor;
        if (mainCaptureExcluded == exclude) return true;
        var handle = WinRT.Interop.WindowNative.GetWindowHandle(this);
        if (SetWindowDisplayAffinity(handle, exclude ? 0x11u : 0u)) { mainCaptureExcluded = exclude; return true; }
        WriteLog("capture-feedback", "Cannot update main-window capture exclusion: " + Marshal.GetLastWin32Error());
        return false;
    }
    private void VerifySharingCaptureExclusion()
    {
        var handle = WinRT.Interop.WindowNative.GetWindowHandle(this);
        foreach (var kind in new CaptureItemKind?[] { CaptureItemKind.Monitor, CaptureItemKind.Window, CaptureItemKind.Monitor, CaptureItemKind.Camera, CaptureItemKind.Monitor, null })
        {
            ApplySharingCaptureExclusion(kind);
            if (!GetWindowDisplayAffinity(handle, out var affinity) || affinity != (kind == CaptureItemKind.Monitor ? 0x11u : 0u))
                throw new InvalidOperationException("Main-window monitor capture exclusion did not follow source selection.");
        }
    }
    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetWindowDisplayAffinity(nint window, uint affinity);
    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetWindowDisplayAffinity(nint window, out uint affinity);
}
