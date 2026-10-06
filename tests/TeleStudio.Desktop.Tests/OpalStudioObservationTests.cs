using TeleStudio.Contracts;
using Xunit;

namespace TeleStudio.Desktop.Tests;

public sealed class OpalStudioObservationTests
{
    [Fact]
    public void A_different_remote_cannot_mark_the_selected_studio_connected()
    {
        var history = new OpalStudioObservations();
        history.Begin("10.136.61.31", CallProtocol.H323);
        history.Apply(new CallConnected("10.136.51.11", CallProtocol.Sip, 1280, 720, 30));
        Assert.Equal("dialing", history.Find("10.136.61.31")!.State);
        Assert.Null(history.Find("10.136.51.11"));
    }

    [Fact]
    public void Terminal_snapshot_is_persisted_and_late_media_does_not_change_it()
    {
        var directory = Directory.CreateTempSubdirectory("MyStudio observation ");
        var file = Path.Combine(directory.FullName, "observations.json");
        var history = new OpalStudioObservations(file);
        history.Begin("10.136.61.31", CallProtocol.H323);
        history.Apply(new CallConnected("h323:10.136.61.31", CallProtocol.H323, 1280, 720, 30));
        history.Apply(new SipConnectionHealthChanged(null, null, null, null, null, null, null, 0, 100, 0, 12, 70, 60));
        history.Apply(new CallEnded("local-hangup"));
        history.Apply(new SipConnectionHealthChanged(null, null, null, null, null, null, null, 0, 999, 0, 999, 999, 999));
        var saved = new OpalStudioObservations(file).Find("10.136.61.31")!;
        Assert.Equal("ended", saved.State);
        Assert.Equal(100, saved.DecodedAudioSamples);
        Assert.Equal(12, saved.DecodedVideoFrames);
        history.Begin("10.136.61.31", CallProtocol.H323);
        Assert.Null(history.Find("10.136.61.31")!.DecodedAudioSamples);
    }

    [Fact]
    public void A_failed_call_stores_no_arbitrary_details_or_credentials()
    {
        var history = new OpalStudioObservations();
        history.Begin("10.136.61.31", CallProtocol.H323);
        history.Apply(new CallFailed("failed password=private-value", "Authorization: sensitive"));
        Assert.Equal("call-failed", history.Find("10.136.61.31")!.FailureCode);
    }

    [Fact]
    public void Export_redacts_snapshot_and_event_secrets()
    {
        var report = OpalSupportReport.Build(new { Password = "snapshot-secret", Token = "token-secret" },
            [new CallFailed("fixture", "Authorization: Bearer event-secret")], [], "password=log-secret", "{}");
        foreach (var secret in new[] { "snapshot-secret", "token-secret", "event-secret", "log-secret" })
            Assert.DoesNotContain(secret, report);
        Assert.Contains("redacted", report);
        Assert.Contains("Recent engine events", report);
    }
}
