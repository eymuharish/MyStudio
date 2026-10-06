using System.Text.Json;
using TeleStudio.Desktop.Audio;

namespace TeleStudio.Desktop.Tests;

public sealed class MicrophoneEvidenceTests
{
    private static readonly MicrophoneTestResult Result =
        new("opaque-endpoint-id", "Surface Microphone", -3.2, -12.4, true, @"C:\secret\mic-test.wav");

    [Fact]
    public void Serialization_contains_only_permitted_fields_and_discards_id_and_wave_path()
    {
        var evidence = MicrophoneEvidence.FromResult(Result, TimeSpan.FromSeconds(3));

        var json = evidence.ToJson();
        using var document = JsonDocument.Parse(json);
        var names = document.RootElement.EnumerateObject().Select(property => property.Name).Order().ToArray();

        Assert.Equal(
            ["deviceName", "durationSeconds", "nonSilent", "peakDbFs", "playbackStatus", "rmsDbFs", "userConfirmed"],
            names);
        Assert.DoesNotContain("opaque-endpoint-id", json, StringComparison.Ordinal);
        Assert.DoesNotContain("mic-test.wav", json, StringComparison.OrdinalIgnoreCase);
        Assert.Equal("STARTED", document.RootElement.GetProperty("playbackStatus").GetString());
        Assert.False(document.RootElement.GetProperty("userConfirmed").GetBoolean());
    }

    [Fact]
    public void Playback_state_requires_started_then_completed_then_human_confirmation()
    {
        var started = MicrophoneEvidence.FromResult(Result, TimeSpan.FromSeconds(3));
        var completed = started.MarkPlaybackCompleted();
        var confirmed = completed.ConfirmPlaybackAudible();

        Assert.Equal("COMPLETED", completed.PlaybackStatus);
        Assert.False(completed.UserConfirmed);
        Assert.Equal("USER_CONFIRMED", confirmed.PlaybackStatus);
        Assert.True(confirmed.UserConfirmed);
        Assert.Throws<InvalidOperationException>(() => started.ConfirmPlaybackAudible());
        Assert.Throws<InvalidOperationException>(() => completed.MarkPlaybackCompleted());
    }

    [Fact]
    public void Evidence_rejects_non_finite_measurements()
    {
        var invalid = Result with { PeakDbFs = double.NaN };

        Assert.Throws<InvalidDataException>(() =>
            MicrophoneEvidence.FromResult(invalid, TimeSpan.FromSeconds(3)));
    }

    [Fact]
    public void Evidence_state_cannot_be_mutated_outside_transition_methods()
    {
        var publicSetters = typeof(MicrophoneEvidence)
            .GetProperties()
            .Where(property => property.SetMethod?.IsPublic == true)
            .Select(property => property.Name);

        Assert.Empty(publicSetters);
    }
}
