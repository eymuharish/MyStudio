using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Security.AccessControl;
using System.Security.Cryptography;
using System.Security.Principal;
using System.Text.Json;

namespace TeleStudio.Desktop.Runtime;

internal sealed record RuntimeSessionRecord(RuntimeProcessIdentity Owner, string Version, string Token,
    IReadOnlyList<RuntimeProcessIdentity> Children);

internal sealed class RuntimeSessionBroker : IDisposable
{
    private readonly string _directory;
    private readonly CancellationTokenSource _lifetime = new();
    private readonly object _recordGate = new();
    private FileStream? _lease;
    private RuntimeSessionRecord? _record;
    private readonly bool _verifyFileSecurity;
    internal Action? ActivateRequested { get; set; }
    internal Func<Task>? ShutdownRequested { get; set; }
    internal bool IsPrimary => _lease is not null;

    internal RuntimeSessionBroker() : this(TeleStudio.Settings.AppDataPaths.OverrideRoot is { } isolated ? Path.Combine(isolated, "Runtime") : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "EssaStudio", "Runtime"), RuntimeDirectorySecurity.Secure)
    { _verifyFileSecurity = true; }

    internal RuntimeSessionBroker(string directory, Action<string> prepareDirectory)
    {
        _directory = directory;
        prepareDirectory(_directory);
    }

    internal static bool SameApplication(RuntimeSessionRecord existing, RuntimeProcessIdentity current, string version) =>
        string.Equals(existing.Owner.ExecutablePath, current.ExecutablePath, StringComparison.OrdinalIgnoreCase) && existing.Version == version;

    internal async Task<bool> AcquireAsync()
    {
        var current = RuntimeProcessIdentity.Capture(Process.GetCurrentProcess());
        var version = FileVersionInfo.GetVersionInfo(current.ExecutablePath).FileVersion ?? "unknown";
        if (!TryLock())
        {
            // The first instance can be between lock acquisition and metadata publication.
            RuntimeSessionRecord? existing = null;
            for (var attempt = 0; attempt < 10 && existing is null; attempt++)
            { existing = ReadRecord(); if (existing is null) await Task.Delay(50); }
            if (existing is null) throw new IOException("Another EssaStudio session has not released its runtime lock.");
            using var verified = existing.Owner.OpenVerified();
            if (verified is not null && SameApplication(existing, current, version))
            {
                if (!await SendAsync(existing, "activate"))
                    throw new IOException("The existing EssaStudio window did not answer. Close it before reopening this version.");
                return false;
            }
            if (!await StopExistingAsync(existing))
                throw new IOException("The previous EssaStudio session could not be stopped safely.");
            if (!TryLock()) throw new IOException("EssaStudio is already starting in another process.");
        }
        var previous = ReadRecord();
        if (previous is not null) TerminateVerifiedChildren(previous);
        _record = new(current, version, Convert.ToHexString(RandomNumberGenerator.GetBytes(32)), []);
        SaveRecord();
        OwnedEngineProcesses.Changed = children => { lock (_recordGate) { _record = _record! with { Children = children }; SaveRecord(); } };
        _ = ListenAsync();
        return true;
    }

    internal async Task<bool> StopForInstallerAsync()
    {
        if (TryLock())
        {
            var stale = ReadRecord();
            if (stale is not null) TerminateVerifiedChildren(stale);
            return true;
        }
        var existing = ReadRecord();
        return existing is not null && await StopExistingAsync(existing);
    }

    private async Task<bool> StopExistingAsync(RuntimeSessionRecord record)
    {
        var deadline = Stopwatch.StartNew();
        await SendAsync(record, "shutdown");
        while (deadline.Elapsed < TimeSpan.FromSeconds(4))
        {
            if (TryLock()) { TerminateVerifiedChildren(record); return true; }
            await Task.Delay(50);
        }
        // Identity is checked again immediately before termination: stale/reused PIDs never match.
        TerminateVerifiedChildren(record);
        using (var owner = record.Owner.OpenVerified())
            try { owner?.Kill(); } catch (InvalidOperationException) { }
        while (deadline.Elapsed < TimeSpan.FromSeconds(5))
        {
            if (TryLock()) return true;
            await Task.Delay(25);
        }
        return false;
    }

    private bool TryLock()
    {
        if (_lease is not null) return true;
        var path = Path.Combine(_directory, "session.lock");
        if (RuntimeDirectorySecurity.EntryAttributes(path) is { } attributes && (attributes & FileAttributes.ReparsePoint) != 0)
            throw new UnauthorizedAccessException("Runtime lock cannot be a link.");
        try { _lease = new FileStream(path, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None); }
        catch (IOException) { return false; }
        try
        {
            if (_verifyFileSecurity) RuntimeDirectorySecurity.ProtectFile(path);
            return true;
        }
        catch { _lease.Dispose(); _lease = null; throw; }
    }

    private RuntimeSessionRecord? ReadRecord()
    {
        try
        {
            var path = Path.Combine(_directory, "session.json");
            if (!File.Exists(path)) return null;
            if ((File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0 || new FileInfo(path).Length > 65536)
                throw new IOException("Invalid runtime identity file.");
            if (_verifyFileSecurity && !RuntimeDirectorySecurity.IsTrusted(new FileInfo(path).GetAccessControl())) return null;
            var result = JsonSerializer.Deserialize<RuntimeSessionRecord>(File.ReadAllText(path));
            return result is { Token.Length: 64 } && result.Children.Count <= 32 ? result : null;
        }
        catch (Exception e) when (e is IOException or JsonException) { return null; }
    }

    private void SaveRecord()
    {
        var temporary = Path.Combine(_directory, "session-" + Guid.NewGuid().ToString("N") + ".new");
        try
        {
            using (var output = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None))
            {
                if (_verifyFileSecurity) RuntimeDirectorySecurity.ProtectFile(temporary);
                JsonSerializer.Serialize(output, _record);
            }
            File.Move(temporary, Path.Combine(_directory, "session.json"), true);
        }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
    }

    private static string PipeName(RuntimeSessionRecord record) => "EssaStudio-runtime-" + record.Owner.ProcessId;

    private static async Task<bool> SendAsync(RuntimeSessionRecord record, string command)
    {
        try
        {
            using var timeout = new CancellationTokenSource(TimeSpan.FromMilliseconds(700));
            using var client = new NamedPipeClientStream(".", PipeName(record), PipeDirection.InOut, PipeOptions.Asynchronous);
            await client.ConnectAsync(timeout.Token);
            using var writer = new StreamWriter(client, leaveOpen: true) { AutoFlush = true };
            using var reader = new StreamReader(client, leaveOpen: true);
            await writer.WriteLineAsync((record.Token + " " + command).AsMemory(), timeout.Token);
            return await reader.ReadLineAsync(timeout.Token) == "ok";
        }
        catch (Exception e) when (e is IOException or OperationCanceledException or UnauthorizedAccessException) { return false; }
    }

    private async Task ListenAsync()
    {
        while (!_lifetime.IsCancellationRequested)
        {
            try
            {
                using var server = new NamedPipeServerStream(PipeName(_record!), PipeDirection.InOut, 1,
                    PipeTransmissionMode.Byte, PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
                await server.WaitForConnectionAsync(_lifetime.Token);
                using var timeout = CancellationTokenSource.CreateLinkedTokenSource(_lifetime.Token);
                timeout.CancelAfter(TimeSpan.FromSeconds(1));
                using var reader = new StreamReader(server, leaveOpen: true);
                using var writer = new StreamWriter(server, leaveOpen: true) { AutoFlush = true };
                var line = await reader.ReadLineAsync(timeout.Token);
                if (line == _record!.Token + " activate")
                { await writer.WriteLineAsync("ok"); ActivateRequested?.Invoke(); }
                else if (line == _record.Token + " shutdown")
                { await writer.WriteLineAsync("ok"); if (ShutdownRequested is not null) _ = ShutdownRequested(); }
            }
            catch (Exception e) when (e is IOException or OperationCanceledException or ObjectDisposedException) { }
        }
    }

    private static void TerminateVerifiedChildren(RuntimeSessionRecord record)
    {
        var root = Path.GetDirectoryName(record.Owner.ExecutablePath)! + Path.DirectorySeparatorChar;
        foreach (var child in record.Children)
        {
            var path = Path.GetFullPath(child.ExecutablePath);
            if (!path.StartsWith(root, StringComparison.OrdinalIgnoreCase)) continue;
            using var process = child.OpenVerified();
            try { process?.Kill(entireProcessTree: true); } catch (InvalidOperationException) { }
        }
    }

    public void Dispose()
    {
        if (_record is not null)
        {
            OwnedEngineProcesses.TerminateChildren();
            OwnedEngineProcesses.Changed = null;
        }
        _lifetime.Cancel();
        _lease?.Dispose();
        _lease = null;
        _lifetime.Dispose();
    }
}
