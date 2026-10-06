using System.Text.Json;
using TeleStudio.Contracts;

namespace TeleStudio.Contracts.Tests;

public sealed class VideoContractsTests
{
    [Fact]
    public void Video_commands_round_trip_and_format_the_frozen_wire_tokens()
    {
        var options = new JsonSerializerOptions(JsonSerializerDefaults.Web);
        EngineCommand[] commands =
        [
            new SetVideoPreferences(VideoResolution.Fhd, 20000),
            new SetCameraEnabled(true, "camera-α"),
            new SetCameraEnabled(false),
            new SetDesktopEnabled(true, "display-1"),
            new SetCameraOverlay(CameraOverlaySize.Large, CameraOverlayPosition.TopLeft),
        ];

        Assert.Equal("video-preferences fhd 20000", VideoCommandCodec.Format((SetVideoPreferences)commands[0]));
        Assert.Equal("camera on Y2FtZXJhLc6x", VideoCommandCodec.Format((SetCameraEnabled)commands[1]));
        Assert.Equal("camera off", VideoCommandCodec.Format((SetCameraEnabled)commands[2]));
        Assert.Equal("desktop on ZGlzcGxheS0x", VideoCommandCodec.Format((SetDesktopEnabled)commands[3]));
        Assert.Equal("camera-overlay large top-left", VideoCommandCodec.Format((SetCameraOverlay)commands[4]));
        foreach (var command in commands)
            Assert.Equal(command, JsonSerializer.Deserialize<EngineCommand>(JsonSerializer.Serialize(command, options), options));
    }

    [Theory]
    [InlineData(127)]
    [InlineData(20001)]
    public void Video_bitrate_outside_manual_range_is_rejected(int bitrate) =>
        Assert.Throws<ArgumentOutOfRangeException>(() => new SetVideoPreferences(VideoResolution.Auto, bitrate));

    [Fact]
    public void Identifier_formatting_rejects_controls_and_oversized_utf8()
    {
        Assert.Throws<ArgumentOutOfRangeException>(() => VideoCommandCodec.Format(new SetDesktopEnabled(true, new string('a', 2049))));
        Assert.Throws<ArgumentOutOfRangeException>(() => VideoCommandCodec.Format(new SetCameraEnabled(true, "bad\0id")));
        Assert.Throws<ArgumentOutOfRangeException>(() => VideoCommandCodec.Format(new SetCameraEnabled(true, "\u0080")));
        Assert.Throws<ArgumentOutOfRangeException>(() => VideoCommandCodec.Format(new SetDesktopEnabled(true, "\u009F")));
    }
}
