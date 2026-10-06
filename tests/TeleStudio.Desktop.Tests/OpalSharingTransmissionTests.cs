using MyStudio.WinUI;
using System.Text.Json;
using TeleStudio.Contracts;
using TeleStudio.Desktop;

namespace TeleStudio.Desktop.Tests;

public sealed class OpalSharingTransmissionTests
{
    [Theory]
    [InlineData(SharingTransmissionMode.Auto, "share-mode auto")]
    [InlineData(SharingTransmissionMode.MainOnly, "share-mode main")]
    [InlineData(SharingTransmissionMode.MainAndPresentation, "share-mode dual")]
    public void Source_selection_carries_the_mode_to_the_opal_worker(SharingTransmissionMode mode, string wire)
    {
        foreach (var kind in new[] { CaptureItemKind.Monitor, CaptureItemKind.Window })
        {
            var source = new CaptureCatalogItem("chosen-source", kind, "Source", 1920, 1080);
            var desktop = Assert.IsType<SetDesktopEnabled>(Assert.Single(OpalSharingPolicy.Select(source, null, mode)));
            Assert.Equal(mode, desktop.TransmissionMode);
            Assert.Equal(wire, OpalEngineCommandSink.SharingModeCommand(desktop.TransmissionMode));
            Assert.Equal(desktop, JsonSerializer.Deserialize<EngineCommand>(JsonSerializer.Serialize<EngineCommand>(desktop)));
        }
    }

    [Fact]
    public void Old_commands_default_to_automatic_and_camera_selection_does_not_set_a_sharing_mode()
    {
        var command = Assert.IsType<SetDesktopEnabled>(JsonSerializer.Deserialize<EngineCommand>("{\"type\":\"setDesktopEnabled\",\"enabled\":true,\"displayId\":\"old-source\"}", new JsonSerializerOptions { PropertyNameCaseInsensitive = true }));
        Assert.Equal(SharingTransmissionMode.Auto, command.TransmissionMode);
        var camera = new CaptureCatalogItem("camera", CaptureItemKind.Camera, "Camera", 0, 0);
        Assert.Equal(new SetCameraEnabled(true, "camera"), Assert.Single(OpalSharingPolicy.Select(camera, null, SharingTransmissionMode.MainOnly)));
        Assert.Throws<ArgumentOutOfRangeException>(() => OpalSharingPolicy.Select(camera, null, (SharingTransmissionMode)99));
        Assert.Throws<ArgumentOutOfRangeException>(() => OpalEngineCommandSink.SharingModeCommand((SharingTransmissionMode)99));
    }
}
