using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace MyStudio.WinUI;

internal static class CallControlVisuals
{
    public static Grid Icon(Symbol symbol, bool crossedOut)
    {
        var icon = new Grid { Width = 22, Height = 22 };
        icon.Children.Add(new SymbolIcon(symbol) { Width = 20, Height = 20 });
        if (crossedOut) icon.Children.Add(new Border
        {
            Width = 28, Height = 2, Background = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 235, 90, 90)),
            RenderTransformOrigin = new Windows.Foundation.Point(.5, .5), RenderTransform = new RotateTransform { Angle = -45 }
        });
        return icon;
    }
    public static void Apply(AppBarButton button, Symbol symbol, CallToolState state)
    {
        button.Label = state.Label;
        button.Icon = new SymbolIcon(symbol);
        if (state.CrossedOut)
        {
            // Explicit paths avoid depending on an undocumented font glyph mapping.
            var shape = symbol switch
            {
                Symbol.Microphone => "M9,2 C9,0 15,0 15,2 L15,11 C15,15 9,15 9,11 Z M5,9 L7,9 L7,11 C7,18 17,18 17,11 L17,9 L19,9 L19,11 C19,16 16,18 13,18 L13,21 L17,21 L17,23 L7,23 L7,21 L11,21 L11,18 C7,18 5,15 5,11 Z",
                Symbol.Camera => "M2,6 L7,6 L9,3 L15,3 L17,6 L22,6 L22,20 L2,20 Z M8,10 L8,16 L16,16 L16,10 Z",
                Symbol.Volume => "M2,9 L6,9 L13,3 L13,21 L6,15 L2,15 Z",
                Symbol.Share => "M2,4 L22,4 L22,18 L13,18 L13,21 L17,21 L17,23 L7,23 L7,21 L11,21 L11,18 L2,18 Z M4,6 L4,16 L20,16 L20,6 Z",
                _ => "M7,3 L20,1 L20,17 C20,23 13,23 13,19 C13,15 16,15 18,15 L18,6 L9,8 L9,20 C9,25 2,25 2,21 C2,17 5,17 7,17 Z"
            };
            button.Icon = (PathIcon)Microsoft.UI.Xaml.Markup.XamlReader.Load("<PathIcon xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' Data='" + shape + " M1,21 L3,23 L23,3 L21,1 Z' />");
        }
        if (state.Active) button.Foreground = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 0, 160, 115));
        else button.ClearValue(Control.ForegroundProperty);
        ToolTipService.SetToolTip(button, state.Description);
        AutomationProperties.SetName(button, state.Description);
        AutomationProperties.SetHelpText(button, state.Description);
    }
    public static void Apply(Button button, Symbol symbol, CallToolState state)
    {
        var content = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
        content.Children.Add(Icon(symbol, state.CrossedOut)); content.Children.Add(new TextBlock { Text = state.Label });
        button.Content = content;
        ToolTipService.SetToolTip(button, state.Description);
        AutomationProperties.SetName(button, state.Description);
    }
}
