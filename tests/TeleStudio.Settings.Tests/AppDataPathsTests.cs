using TeleStudio.Settings;

namespace TeleStudio.Settings.Tests;

public sealed class AppDataPathsTests
{
    [Fact]
    public void Explicit_test_root_is_absolute_and_all_test_stores_stay_under_it()
    {
        var root = Path.Combine(Path.GetTempPath(), "EssaStudio isolated");
        Assert.Equal(Path.GetFullPath(root), AppDataPaths.ResolveRoot(root));
        Assert.Throws<ArgumentException>(() => AppDataPaths.ResolveRoot("relative-test"));
    }

    [Fact]
    public void Startup_data_root_switch_is_consumed_without_breaking_recovery_arguments()
    {
        var root = Path.Combine(Path.GetTempPath(), "isolated");
        var (remaining, selected) = AppDataPaths.ParseArguments(["--data-root", root, "--shutdown"]);
        Assert.Equal(["--shutdown"], remaining);
        Assert.Equal(root, selected);
        Assert.Throws<ArgumentException>(() => AppDataPaths.ParseArguments(["--data-root"]));
    }
}
