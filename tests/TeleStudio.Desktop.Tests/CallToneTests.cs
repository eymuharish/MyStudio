using TeleStudio.Contracts;

namespace TeleStudio.Desktop.Tests;

public class CallToneTests
{
    [Fact]
    public async Task Queued_events_after_disposal_cannot_restart_audio()
    {
        var output=new FakeOutput(); var tones=new CallToneController(output);
        tones.Dispose();
        tones.Observe(new CallProgress("192.0.2.1",CallProtocol.Sip,CallState.Calling));
        await tones.EndAsync();
        Assert.Empty(output.Played);
    }
    [Fact]
    public async Task Progress_rings_once_and_connection_stops_it_without_ending_tone()
    {
        var output = new FakeOutput(); using var tones = new CallToneController(output);
        tones.Observe(new CallProgress("192.0.2.1", CallProtocol.Sip, CallState.Calling));
        tones.Observe(new CallProgress("192.0.2.1", CallProtocol.Sip, CallState.Negotiating));
        Assert.Equal(new[]{CallTone.Dialing}, output.Played);
        tones.Observe(new CallConnected("192.0.2.1", CallProtocol.Sip, 0,0,0));
        Assert.True(output.Tokens[0].IsCancellationRequested);
        await tones.EndAsync();
        tones.Observe(new CallEnded("remote-ended"));
        Assert.Equal(new[]{CallTone.Dialing,CallTone.Ended}, output.Played);
    }

    [Fact]
    public async Task Idle_close_is_silent_but_incoming_call_end_plays_once()
    {
        var output=new FakeOutput(); using var tones=new CallToneController(output);
        await tones.EndAsync(); Assert.Empty(output.Played);
        tones.Observe(new IncomingCall("192.0.2.1",CallProtocol.Sip));
        tones.Observe(new CallConnected("192.0.2.1",CallProtocol.Sip,0,0,0));
        await tones.EndAsync(); await tones.EndAsync();
        Assert.Equal(new[]{CallTone.Ended},output.Played);
    }

    [Theory]
    [InlineData(CallTone.Dialing,2.0)]
    [InlineData(CallTone.Ended,0.45)]
    public void Generated_pcm_has_bounded_duration_and_non_silent_audio(CallTone tone,double seconds)
    {
        var pcm=CallTonePcm.Create(tone);
        Assert.Equal((int)(CallTonePcm.SampleRate*seconds),pcm.Length);
        Assert.Contains(pcm,s=>Math.Abs(s)>.01f);
        Assert.All(pcm,s=>Assert.InRange(s,-.25f,.25f));
        Assert.Equal(0,pcm[0]);
    }
    private sealed class FakeOutput:ICallToneOutput
    {
        public List<CallTone> Played {get;}=[];
        public List<CancellationToken> Tokens {get;}=[];
        public Task PlayAsync(CallTone tone,CancellationToken token)
        { Played.Add(tone);Tokens.Add(token);return Task.CompletedTask; }
    }
}
