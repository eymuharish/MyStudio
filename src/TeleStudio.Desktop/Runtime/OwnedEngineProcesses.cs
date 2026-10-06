using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace TeleStudio.Desktop.Runtime;

internal sealed record RuntimeProcessIdentity(int ProcessId, long StartUtcTicks, string ExecutablePath)
{
    internal static RuntimeProcessIdentity Capture(Process process)
    {
        // During loader initialization MainModule can transiently be ntdll.dll.
        // The kernel image identifies the executable without waiting for its module list.
        var buffer = new StringBuilder(32768);
        var length = (uint)buffer.Capacity;
        if (!QueryFullProcessImageName(process.SafeHandle, 0, buffer, ref length))
            throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(), "Could not verify the native process image.");
        var image = buffer.ToString();
        return new(process.Id, process.StartTime.ToUniversalTime().Ticks, Path.GetFullPath(image));
    }

    [DllImport("kernel32.dll", EntryPoint = "QueryFullProcessImageNameW", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool QueryFullProcessImageName(SafeProcessHandle process, uint flags, StringBuilder image, ref uint length);

    internal static bool Matches(RuntimeProcessIdentity expected, RuntimeProcessIdentity actual) =>
        expected.ProcessId == actual.ProcessId && expected.StartUtcTicks == actual.StartUtcTicks &&
        string.Equals(Path.GetFullPath(expected.ExecutablePath), Path.GetFullPath(actual.ExecutablePath), StringComparison.OrdinalIgnoreCase);

    internal Process? OpenVerified()
    {
        Process? process = null;
        try
        {
            process = Process.GetProcessById(ProcessId);
            if (!process.HasExited && Matches(this, Capture(process))) return process;
        }
        catch (Exception e) when (e is ArgumentException or InvalidOperationException or System.ComponentModel.Win32Exception) { }
        process?.Dispose();
        return null;
    }
}

/// <summary>Only children launched by this application are enrolled. Closing its job also
/// cleans descendants after a crash; persisted identities are a guarded recovery fallback.</summary>
internal static class OwnedEngineProcesses
{
    private static readonly object Gate = new();
    private static readonly Dictionary<int, RuntimeProcessIdentity> Children = new();
    private static SafeFileHandle? _job;
    internal static Action<IReadOnlyList<RuntimeProcessIdentity>>? Changed { get; set; }

    internal static void Register(Process process)
    {
        lock (Gate)
        {
            // Unit-test console consumers do not claim application-wide ownership.
            if (Changed is null) return;
            _job ??= CreateKillOnCloseJob();
            if (!AssignProcessToJobObject(_job, process.Handle))
                throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(), "Could not isolate the native engine lifetime.");
            Children[process.Id] = RuntimeProcessIdentity.Capture(process);
            Changed(Children.Values.ToArray());
        }
    }

    internal static void Unregister(int processId)
    {
        lock (Gate)
            if (Children.Remove(processId)) Changed?.Invoke(Children.Values.ToArray());
    }

    internal static void TerminateChildren()
    {
        lock (Gate)
        {
            if (Changed is null) return;
            _job?.Dispose();
            _job = null;
            foreach (var child in Children.Values)
            {
                using var process = child.OpenVerified();
                try { process?.Kill(entireProcessTree: true); } catch (InvalidOperationException) { }
            }
            Children.Clear();
            Changed(Children.Values.ToArray());
        }
    }

    private static SafeFileHandle CreateKillOnCloseJob()
    {
        var job = CreateJobObject(IntPtr.Zero, null);
        if (job.IsInvalid) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        var limits = new ExtendedLimitInformation();
        limits.BasicLimitInformation.LimitFlags = 0x2000; // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        var size = Marshal.SizeOf<ExtendedLimitInformation>();
        var buffer = Marshal.AllocHGlobal(size);
        try
        {
            Marshal.StructureToPtr(limits, buffer, false);
            if (!SetInformationJobObject(job, 9, buffer, (uint)size))
                throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
            return job;
        }
        catch { job.Dispose(); throw; }
        finally { Marshal.FreeHGlobal(buffer); }
    }

    [StructLayout(LayoutKind.Sequential)] private struct BasicLimitInformation
    {
        public long PerProcessUserTimeLimit, PerJobUserTimeLimit;
        public uint LimitFlags;
        public UIntPtr MinimumWorkingSetSize, MaximumWorkingSetSize;
        public uint ActiveProcessLimit;
        public UIntPtr Affinity;
        public uint PriorityClass, SchedulingClass;
    }
    [StructLayout(LayoutKind.Sequential)] private struct IoCounters
    { public ulong ReadOperationCount, WriteOperationCount, OtherOperationCount, ReadTransferCount, WriteTransferCount, OtherTransferCount; }
    [StructLayout(LayoutKind.Sequential)] private struct ExtendedLimitInformation
    {
        public BasicLimitInformation BasicLimitInformation;
        public IoCounters IoInfo;
        public UIntPtr ProcessMemoryLimit, JobMemoryLimit, PeakProcessMemoryUsed, PeakJobMemoryUsed;
    }
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern SafeFileHandle CreateJobObject(IntPtr attributes, string? name);
    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)] private static extern bool SetInformationJobObject(SafeFileHandle job, int infoClass, IntPtr info, uint length);
    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)] private static extern bool AssignProcessToJobObject(SafeFileHandle job, IntPtr process);
}
