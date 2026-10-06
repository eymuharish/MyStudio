using System.IO;
using System.Security.AccessControl;
using System.Security.Principal;
using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace TeleStudio.Desktop.Runtime;

internal static class RuntimeDirectorySecurity
{
    private static readonly SecurityIdentifier Administrators = new(WellKnownSidType.BuiltinAdministratorsSid, null);
    private static readonly SecurityIdentifier SystemAccount = new(WellKnownSidType.LocalSystemSid, null);

    internal static bool CanAdoptOwner(SecurityIdentifier owner, SecurityIdentifier currentUser, FileAttributes attributes) =>
        (attributes & FileAttributes.ReparsePoint) == 0 &&
        (owner.Equals(currentUser) || owner.Equals(Administrators) || owner.Equals(SystemAccount));

    internal static bool IsTrusted(FileSystemSecurity security) =>
        security.AreAccessRulesProtected &&
        (Administrators.Equals(security.GetOwner(typeof(SecurityIdentifier))) || SystemAccount.Equals(security.GetOwner(typeof(SecurityIdentifier)))) &&
        security.GetAccessRules(true, true, typeof(SecurityIdentifier)).Cast<FileSystemAccessRule>().All(rule =>
            rule.AccessControlType != AccessControlType.Allow || rule.IdentityReference.Equals(Administrators) || rule.IdentityReference.Equals(SystemAccount));

    internal static void Secure(string directory)
    {
        var runtime = new DirectoryInfo(directory);
        var appDirectory = runtime.Parent ?? throw new IOException("Runtime parent is missing.");
        var ancestry = new List<DirectoryInfo>();
        for (var item = runtime; item is not null; item = item.Parent) ancestry.Add(item);
        ancestry.Reverse();
        var handles = new List<SafeFileHandle>();
        var legacy = false;
        var currentUser = WindowsIdentity.GetCurrent().User ?? throw new UnauthorizedAccessException("Current Windows user is unavailable.");
        try
        {
            // No FILE_SHARE_DELETE: every ancestor stays pinned while path-based ACL/file operations run.
            foreach (var item in ancestry)
            {
                if (EntryAttributes(item.FullName) is { } existingAttributes && (existingAttributes & FileAttributes.ReparsePoint) != 0)
                    throw new IOException("The EssaStudio runtime directory must not be a link.");
                if (!item.Exists) item.Create();
                var handle = CreateFile(item.FullName, 0x20080, 3, IntPtr.Zero, 3, 0x02200000, IntPtr.Zero);
                if (handle.IsInvalid) { handle.Dispose(); throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot secure runtime directory."); }
                handles.Add(handle);
                if (!GetFileInformationByHandleEx(handle, 9, out var information, 8)) throw new Win32Exception(Marshal.GetLastWin32Error());
                var attributes = (FileAttributes)information.Attributes;
                if ((attributes & FileAttributes.ReparsePoint) != 0) throw new IOException("The EssaStudio runtime directory must not be a link.");
                if (!item.FullName.Equals(appDirectory.FullName, StringComparison.OrdinalIgnoreCase) &&
                    !item.FullName.Equals(runtime.FullName, StringComparison.OrdinalIgnoreCase)) continue;
                var previous = item.GetAccessControl(AccessControlSections.Access | AccessControlSections.Owner);
                if (previous.GetOwner(typeof(SecurityIdentifier)) is not SecurityIdentifier owner)
                    throw new UnauthorizedAccessException("The runtime directory owner could not be verified.");
                if (!CanAdoptOwner(owner, currentUser, attributes)) throw new UnauthorizedAccessException("The runtime directory has a foreign owner; automatic migration was refused.");
                legacy |= !IsTrusted(previous);
                var secured = new DirectorySecurity();
                secured.SetOwner(Administrators);
                secured.SetAccessRuleProtection(true, false);
                foreach (var sid in new[] { Administrators, SystemAccount })
                    secured.AddAccessRule(new FileSystemAccessRule(sid, FileSystemRights.FullControl,
                        InheritanceFlags.ContainerInherit | InheritanceFlags.ObjectInherit, PropagationFlags.None, AccessControlType.Allow));
                item.SetAccessControl(secured);
            }
            foreach (var name in new[] { "session.json", "session.json.new", "session.lock" })
            {
                var path = Path.Combine(directory, name);
                if (EntryAttributes(path) is { } attributes)
                    legacy |= (attributes & FileAttributes.ReparsePoint) != 0 || !IsTrusted(new FileInfo(path).GetAccessControl());
            }
            if (legacy) QuarantineLegacyFiles(directory);
        }
        finally { for (var index = handles.Count - 1; index >= 0; index--) handles[index].Dispose(); }
    }

    internal static void ProtectFile(string path)
    {
        var secured = new FileSecurity();
        secured.SetOwner(Administrators);
        secured.SetAccessRuleProtection(true, false);
        foreach (var sid in new[] { Administrators, SystemAccount })
            secured.AddAccessRule(new FileSystemAccessRule(sid, FileSystemRights.FullControl, AccessControlType.Allow));
        new FileInfo(path).SetAccessControl(secured);
    }

    internal static void QuarantineLegacyFiles(string directory)
    {
        var names = new[] { "session.lock", "session.json", "session.json.new" };
        var quarantine = Path.Combine(directory, "untrusted-legacy-" + Guid.NewGuid().ToString("N"));
        foreach (var name in names)
        {
            var path = Path.Combine(directory, name);
            if (EntryAttributes(path) is null) continue;
            Directory.CreateDirectory(quarantine);
            // A running legacy process holds the lock without delete sharing: refuse migration until it closes.
            File.Move(path, Path.Combine(quarantine, name));
        }
    }

    internal static FileAttributes? EntryAttributes(string path)
    {
        try { return File.GetAttributes(path); }
        catch (FileNotFoundException) { return null; }
        catch (DirectoryNotFoundException) { return null; }
    }

    [StructLayout(LayoutKind.Sequential)] private struct AttributeTagInformation { public uint Attributes; public uint ReparseTag; }
    [DllImport("kernel32.dll", EntryPoint = "CreateFileW", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern SafeFileHandle CreateFile(string path, uint access, uint share, IntPtr security, uint creation, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetFileInformationByHandleEx(SafeFileHandle handle, int informationClass, out AttributeTagInformation information, uint size);
}
