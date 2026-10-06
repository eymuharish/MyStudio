using System.Security.Principal;
using System.Security.AccessControl;
using TeleStudio.Desktop.Runtime;

namespace TeleStudio.Desktop.Tests;

public sealed class RuntimeDirectorySecurityTests
{
    [Fact]
    public void Legacy_current_user_or_system_owner_is_adoptable_but_foreign_owner_or_reparse_is_denied()
    {
        var current = new SecurityIdentifier("S-1-5-21-100-200-300-1001");
        Assert.True(RuntimeDirectorySecurity.CanAdoptOwner(current, current, FileAttributes.Directory));
        Assert.True(RuntimeDirectorySecurity.CanAdoptOwner(new(WellKnownSidType.BuiltinAdministratorsSid, null), current, FileAttributes.Directory));
        Assert.True(RuntimeDirectorySecurity.CanAdoptOwner(new(WellKnownSidType.LocalSystemSid, null), current, FileAttributes.Directory));
        Assert.False(RuntimeDirectorySecurity.CanAdoptOwner(new("S-1-5-21-100-200-300-1002"), current, FileAttributes.Directory));
        Assert.False(RuntimeDirectorySecurity.CanAdoptOwner(current, current, FileAttributes.Directory | FileAttributes.ReparsePoint));
    }

    [Fact]
    public void Migration_quarantines_only_runtime_identity_and_lock_preserving_other_files()
    {
        var directory = Directory.CreateTempSubdirectory("EssaStudio-runtime-migration-test-");
        try
        {
            foreach (var name in new[] { "session.json", "session.json.new", "session.lock", "recording.mp4", "settings.json" })
                File.WriteAllText(Path.Combine(directory.FullName, name), name);
            RuntimeDirectorySecurity.QuarantineLegacyFiles(directory.FullName);
            Assert.False(File.Exists(Path.Combine(directory.FullName, "session.json")));
            Assert.False(File.Exists(Path.Combine(directory.FullName, "session.lock")));
            Assert.Equal("recording.mp4", File.ReadAllText(Path.Combine(directory.FullName, "recording.mp4")));
            Assert.Equal("settings.json", File.ReadAllText(Path.Combine(directory.FullName, "settings.json")));
            Assert.Equal(5, Directory.GetFiles(directory.FullName, "*", SearchOption.AllDirectories).Length);
        }
        finally { directory.Delete(true); }
    }

    [Fact]
    public void Current_user_owned_or_users_writable_records_are_not_authenticated_runtime_records()
    {
        var administrators = new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null);
        var current = new SecurityIdentifier("S-1-5-21-100-200-300-1001");
        var security = new FileSecurity();
        security.SetAccessRuleProtection(true, false);
        security.SetOwner(administrators);
        security.AddAccessRule(new FileSystemAccessRule(administrators, FileSystemRights.FullControl, AccessControlType.Allow));
        Assert.True(RuntimeDirectorySecurity.IsTrusted(security));
        security.SetOwner(current);
        Assert.False(RuntimeDirectorySecurity.IsTrusted(security));
        security.SetOwner(administrators);
        security.AddAccessRule(new FileSystemAccessRule(new SecurityIdentifier(WellKnownSidType.BuiltinUsersSid, null), FileSystemRights.Write, AccessControlType.Allow));
        Assert.False(RuntimeDirectorySecurity.IsTrusted(security));
    }

    [Fact]
    public void Locked_legacy_session_aborts_migration_without_using_or_moving_its_identity()
    {
        var directory = Directory.CreateTempSubdirectory("EssaStudio-runtime-migration-test-");
        try
        {
            var record = Path.Combine(directory.FullName, "session.json");
            File.WriteAllText(record, "Untrusted identity");
            using var locked = new FileStream(Path.Combine(directory.FullName, "session.lock"), FileMode.CreateNew, FileAccess.ReadWrite, FileShare.None);
            Assert.Throws<IOException>(() => RuntimeDirectorySecurity.QuarantineLegacyFiles(directory.FullName));
            Assert.Equal("Untrusted identity", File.ReadAllText(record));
        }
        finally { directory.Delete(true); }
    }
}
