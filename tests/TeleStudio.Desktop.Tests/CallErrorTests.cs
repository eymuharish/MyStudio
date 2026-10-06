using TeleStudio.Contracts;
using TeleStudio.Desktop;
using TeleStudio.Presentation;
namespace TeleStudio.Desktop.Tests;
public class CallErrorTests
{
    [Theory]
    [InlineData("486 Busy Here")]
    [InlineData("600 Busy Everywhere")]
    [InlineData("EndedByRemoteBusy")]
    [InlineData("Busy")]
    public void Terminal_busy_is_actionable(string reason)
    {
        var notice = CallErrorDescription.FromEnded(reason, AppLanguage.Arabic);
        Assert.Equal("busy", notice.Kind);
        Assert.Contains("مشغول", notice.Message);
        Assert.True(notice.AllowRetry);
        Assert.Equal(reason, notice.Detail);
    }
    [Theory]
    [InlineData("transport-fail", "unavailable")]
    [InlineData("connect-fail", "unavailable")]
    [InlineData("no-endpoint", "unavailable")]
    [InlineData("no-feature", "media")]
    [InlineData("no-bandwidth", "unavailable")]
    [InlineData("no-user", "address")]
    [InlineData("capability", "media")]
    [InlineData("remote-busy", "busy")]
    [InlineData("remote-user", "remote")]
    [InlineData("403 Forbidden", "denied")]
    [InlineData("401 Unauthorized", "authentication")]
    [InlineData("407 Proxy Authentication Required", "authentication")]
    [InlineData("404 Not Found", "address")]
    [InlineData("408 Request Timeout", "timeout")]
    [InlineData("488 Not Acceptable Here", "media")]
    [InlineData("503 Service Unavailable", "unavailable")]
    public void Other_reasons_are_not_hidden(string reason, string kind)
    {
        Assert.Equal(kind, CallErrorDescription.FromEnded(reason, AppLanguage.English).Kind);
    }
    [Fact]
    public void Unknown_reason_preserved_without_guessing()
    {
        var notice = CallErrorDescription.FromEnded("vendor-unknown-42", AppLanguage.English);
        Assert.Equal("unknown", notice.Kind);
        Assert.Equal("vendor-unknown-42", notice.Detail);
    }
    [Fact]
    public void Cleanup_failure_requires_recovery_before_retry()
    {
        Assert.False(CallErrorDescription.FromFailed(new("sip-call-cleanup-failed"), AppLanguage.Arabic).AllowRetry);
    }
    [Theory]
    [InlineData("local-hangup")]
    [InlineData("cancelled")]
    [InlineData("local-user")]
    public void Local_actions_are_not_errors(string reason) => Assert.True(CallErrorDescription.IsLocalEnd(reason));
    [Theory]
    [InlineData("declined")]
    [InlineData("rejected")]
    public void Remote_rejection_is_not_assumed_to_be_local(string reason)
    {
        Assert.False(CallErrorDescription.IsLocalEnd(reason));
        Assert.Equal("denied",CallErrorDescription.FromEnded(reason,AppLanguage.Arabic).Kind);
    }
    [Fact]
    public void Retry_requires_terminal_state_and_original_destination()
    {
        Assert.True(CallErrorDescription.CanRetry(CallState.Idle, true, false, "studio", "studio"));
        Assert.True(CallErrorDescription.CanRetry(CallState.Failed, true, false, "studio", "studio"));
        Assert.False(CallErrorDescription.CanRetry(CallState.ConnectedCamera, true, false, "studio", "studio"));
        Assert.False(CallErrorDescription.CanRetry(CallState.Ending, true, false, "studio", "studio"));
        Assert.False(CallErrorDescription.CanRetry(CallState.Idle, true, false, "studio", "another"));
        Assert.False(CallErrorDescription.CanRetry(CallState.Idle, false, false, "studio", "studio"));
    }
}
