using TeleStudio.Contracts;
using MyStudio.WinUI;

namespace TeleStudio.Desktop.Tests;

public sealed class MyStudioPolicyTests
{
    [Fact]
    public void Connected_without_native_capabilities_does_not_enable_media_tools()
    {
        var policy = StudioControlPolicy.From(true, null, RecordingState.Idle);
        Assert.False(policy.Camera); Assert.False(policy.Audio); Assert.False(policy.Record);
    }
    [Fact]
    public void Recording_can_stop_after_call_ends_without_enabling_new_recording()
    {
        var policy = StudioControlPolicy.From(false, null, RecordingState.Finalizing);
        Assert.False(policy.Record); Assert.False(policy.StopRecording);
        Assert.True(StudioControlPolicy.From(false, null, RecordingState.Recording).StopRecording);
    }
    [Fact]
    public void Recording_capability_alone_does_not_enable_recording_without_confirmed_sharing()
    {
        var capabilities = new MediaCapabilitiesChanged(true, true, true, true, true, true, true, true);
        Assert.False(StudioControlPolicy.From(true, capabilities, RecordingState.Idle).Record);
        Assert.False(StudioControlPolicy.From(true, capabilities, RecordingState.Completed).Record);
    }
    [Fact]
    public void Codec_preferences_cannot_serialize_a_g711_or_undefined_selection()
    {
        var settings = new ConnectionTuning { AudioCodec = (AudioSelection)999 };
        Assert.Throws<ArgumentException>(() => settings.Validate(EngineFamily.Cisco, CallProtocol.Sip));
        Assert.Equal("audio-profile g722 1 64 100 300 100 40 0", (new ConnectionTuning { AudioCodec = AudioSelection.G722 }).AudioCommand);
    }
}
