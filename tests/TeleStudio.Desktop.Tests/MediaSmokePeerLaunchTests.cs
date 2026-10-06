using MyStudio.WinUI;

namespace TeleStudio.Desktop.Tests;

public sealed class MediaSmokePeerLaunchTests
{
    [Fact]
    public void Normal_smoke_keeps_the_real_worker_and_arguments()
    {
        Assert.Equal(new MediaSmokePeerLaunch("worker.exe", "--sip-port 5141"),
            MediaSmokePeerLaunch.Create("worker.exe", "--sip-port 5141", []));
    }

    [Fact]
    public void Debugger_wraps_only_the_peer_with_a_quoted_real_worker_and_35_second_probe()
    {
        var directory = Directory.CreateTempSubdirectory("OPAL peer diagnostic ");
        try
        {
            var worker = Path.Combine(directory.FullName, "real worker.exe");
            var debugger = Path.Combine(directory.FullName, "opal_debug_probe.exe");
            File.WriteAllText(worker, "fixture"); File.WriteAllText(debugger, "fixture");
            var launch = MediaSmokePeerLaunch.Create(worker, "--sip-port 5141 --h323-port 1761", ["--peer-debugger", debugger]);
            Assert.Equal(debugger, launch.Executable);
            Assert.Equal($"--stacks-after-ms 35000 \"{worker}\" --sip-port 5141 --h323-port 1761", launch.Arguments);
        }
        finally { directory.Delete(true); }
    }

    [Theory]
    [InlineData(null)]
    [InlineData("relative.exe")]
    public void Missing_or_relative_debugger_option_is_rejected(string? argument)
    {
        var arguments = argument is null ? new[] { "--peer-debugger" } : new[] { "--peer-debugger", argument };
        Assert.Throws<ArgumentException>(() => MediaSmokePeerLaunch.Create("worker.exe", "", arguments));
    }
}
