using System.Text;
using TeleStudio.Contracts;
namespace TeleStudio.Desktop.Tests;

public class RoomNoiseReductionTests
{
    [Theory]
    [InlineData("",true,0)]
    [InlineData(",\"noiseReductionDb\":0",true,0)]
    [InlineData(",\"noiseReductionDb\":30",true,30)]
    [InlineData(",\"noiseReductionDb\":31",false,0)]
    [InlineData(",\"noiseReductionDb\":-1",false,0)]
    [InlineData(",\"noiseReductionDb\":1.5",false,0)]
    [InlineData(",\"noiseReductionDb\":15,\"noiseReductionDb\":20",false,0)]
    public void Legacy_and_bounded_noise_states(string suffix,bool accepted,int expected)
    {
        var json="{\"schema\":1,\"type\":\"room-mixer-changed\",\"sequence\":1,\"protocol\":\"sip\",\"masterPercent\":100,\"bassDb\":0,\"voiceDb\":0,\"trebleDb\":0"+suffix+"}\n";
        Assert.Equal(accepted,NativeJsonEvent.TryParse(Encoding.UTF8.GetBytes(json),CallProtocol.Sip,out var parsed,out _));
        if(accepted) Assert.Equal(expected,Assert.IsType<RoomMixerChanged>(parsed!.Event).NoiseReductionDb);
    }
    [Theory]
    [InlineData("sip", CallProtocol.Sip)]
    [InlineData("h323", CallProtocol.H323)]
    public void Accepts_confirmed_room_noise_reduction(string protocol, CallProtocol kind)
    {
        var json="{\"schema\":1,\"type\":\"room-mixer-changed\",\"sequence\":1,\"protocol\":\""+protocol+"\",\"masterPercent\":100,\"bassDb\":0,\"voiceDb\":0,\"trebleDb\":0,\"noiseReductionDb\":15}";
        Assert.True(NativeJsonEvent.TryParse(Encoding.UTF8.GetBytes(json+"\n"),kind,out var parsed,out var reason),reason);
        Assert.Equal(15,Assert.IsType<RoomMixerChanged>(parsed!.Event).NoiseReductionDb);
    }
}
