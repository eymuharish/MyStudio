namespace TeleStudio.Desktop.Tests;

public sealed class StudioTransitionSmokePlanTests
{
    [Fact]
    public void Default_plan_is_four_calls_to_the_authorized_sony_target()
    {
        var plan = StudioTransitionSmokePlan.Create(null, null);
        Assert.Equal(4, plan.Calls); Assert.Equal("10.136.51.11", plan.SonyAddress);
    }
    [Fact]
    public void Explicit_eight_calls_and_authorized_alternate_are_preserved()
    {
        var plan = StudioTransitionSmokePlan.Create("8", "10.136.61.31");
        Assert.Equal(8, plan.Calls); Assert.Equal("10.136.61.31", plan.SonyAddress);
    }
    [Theory]
    [InlineData("0", null)] [InlineData("6", null)] [InlineData("16", null)]
    [InlineData("4", "192.0.2.1")] [InlineData("8", "10.136.51.25")]
    public void Unbounded_or_unauthorized_physical_plans_are_rejected(string count, string? address) =>
        Assert.Throws<ArgumentException>(() => StudioTransitionSmokePlan.Create(count, address));
    [Fact]
    public void Missing_or_default_data_root_is_rejected_before_any_call()
    {
        var root = Path.Combine(Path.GetTempPath(), "studio-normal-root");
        Assert.Throws<ArgumentException>(() => StudioTransitionSmokePlan.RequireIsolation(null, root));
        Assert.Throws<ArgumentException>(() => StudioTransitionSmokePlan.RequireIsolation(root + Path.DirectorySeparatorChar, root));
        var isolated = root + "-isolated";
        Assert.Equal(Path.GetFullPath(isolated), StudioTransitionSmokePlan.RequireIsolation(isolated, root));
    }
}
