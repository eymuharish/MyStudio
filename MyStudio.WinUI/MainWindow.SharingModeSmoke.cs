using Microsoft.UI.Xaml.Controls;
using TeleStudio.Contracts;
using TeleStudio.Desktop;

namespace MyStudio.WinUI;

public sealed partial class MainWindow
{
    private void VerifySharingTransmissionDialogForSmoke(ContentDialog dialog, StackPanel panel)
    {
        var choices = RoomDialogElements(panel).OfType<ComboBox>().ToArray();
        if (choices.Length != 2 || choices[0].Name != "SharingTransmissionMode" || choices[1].Name != "SharingSource")
            throw new InvalidOperationException("Transmission mode must precede source selection.");
        var mode = choices[0]; var source = choices[1];
        var options = mode.Items.OfType<SharingModeOption>().ToArray();
        if (!options.Select(x => x.Mode).SequenceEqual(Enum.GetValues<SharingTransmissionMode>()) ||
            mode.SelectedItem is not SharingModeOption { Mode: SharingTransmissionMode.Auto })
            throw new InvalidOperationException("Sharing modes or automatic default are missing.");
        if (source.SelectedIndex != -1 || dialog.IsPrimaryButtonEnabled)
            throw new InvalidOperationException("Sharing enabled without an explicit source.");
        foreach (var option in options) mode.SelectedItem = option;
        mode.SelectedItem = options.Single(x => x.Mode == SharingTransmissionMode.MainOnly);
        var monitor = source.Items.OfType<SharingOption>().First(x => x.Item.Kind == CaptureItemKind.Monitor);
        source.SelectedItem = monitor;
        if (!dialog.IsPrimaryButtonEnabled || !mode.IsEnabled)
            throw new InvalidOperationException("Screen/window sharing mode is unavailable.");
        var camera = source.Items.OfType<SharingOption>().FirstOrDefault(x => x.Item.Kind == CaptureItemKind.Camera);
        if (camera is not null)
        {
            source.SelectedItem = camera;
            if (mode.IsEnabled) throw new InvalidOperationException("Screen transmission mode applies to camera selection.");
        }
        source.SelectedIndex = -1;
        if (dialog.IsPrimaryButtonEnabled || sharingTransmissionMode != SharingTransmissionMode.Auto)
            throw new InvalidOperationException("Unapplied mode/source selections changed the session.");
    }
}
