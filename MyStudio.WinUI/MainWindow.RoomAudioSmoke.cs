using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation.Peers;
using Microsoft.UI.Xaml.Automation.Provider;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace MyStudio.WinUI;

public sealed partial class MainWindow
{
    private static IEnumerable<DependencyObject> RoomDialogElements(DependencyObject root)
    {
        yield return root;
        for (var i = 0; i < VisualTreeHelper.GetChildrenCount(root); i++)
            foreach (var child in RoomDialogElements(VisualTreeHelper.GetChild(root, i))) yield return child;
    }

    private void VerifyRoomAudioControlsForSmoke(StackPanel panel)
    {
        var elements = RoomDialogElements(panel).ToArray();
        var volume = elements.OfType<NumberBox>().First();
        if (volume.Maximum != 300 || volume.Value != 300)
            throw new InvalidOperationException("Incoming room volume must expose 300% and preserve the confirmed boost when reopened.");
        NumberBox NumberNamed(string name) => elements.OfType<NumberBox>().Single(x => x.Name == name);
        var mic = NumberNamed("MicrophoneGain");
        var originalMic = mic.Value;
        var originalNoise = elements.OfType<CheckBox>().First().IsChecked;
        foreach (var preset in new[] { ("RoomPresetVoice", 150, -3, 4, 2), ("RoomPresetWeak", 200, -4, 3, 1), ("RoomPresetFlat", 100, 0, 0, 0) })
        {
            var button = elements.OfType<Button>().Single(x => x.Name == preset.Item1);
            ((IInvokeProvider)new ButtonAutomationPeer(button).GetPattern(PatternInterface.Invoke)).Invoke();
            if (volume.Value != preset.Item2 || NumberNamed("RoomBass").Value != preset.Item3 ||
                NumberNamed("RoomVoice").Value != preset.Item4 || NumberNamed("RoomTreble").Value != preset.Item5 ||
                mic.Value != originalMic || elements.OfType<CheckBox>().First().IsChecked != originalNoise)
                throw new InvalidOperationException("Room preset did not set the incoming EQ or changed microphone controls.");
        }
        volume.Value = 300;
        NumberNamed("RoomVoice").Value = 6;
        if (volume.Value != 300 || NumberNamed("RoomVoice").Value != 6)
            throw new InvalidOperationException("Manual incoming boost/EQ controls are unavailable.");
    }

    private static void ApplyRoomAudioDialogForSmoke(ContentDialog dialog)
    {
        var apply = RoomDialogElements(dialog).OfType<Button>().Single(x => x.Name == "PrimaryButton");
        ((IInvokeProvider)new ButtonAutomationPeer(apply).GetPattern(PatternInterface.Invoke)).Invoke();
    }
}
