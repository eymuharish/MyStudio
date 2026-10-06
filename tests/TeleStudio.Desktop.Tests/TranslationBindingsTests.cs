using System.Runtime.CompilerServices;

namespace TeleStudio.Desktop.Tests;

public sealed class TranslationBindingsTests
{
    private sealed class Label { public string Text = ""; }
    [Fact]
    public void Closed_dialog_bindings_are_pruned_while_permanent_language_updates_remain()
    {
        var bindings = new TranslationBindings();
        var permanent = new Label(); bindings.Add((value => permanent.Text = value, "عربي", "English"));
        using (bindings.BeginScope())
        {
            for (var i = 0; i < 100; ++i) { var dialog = new Label(); bindings.Add((value => dialog.Text = value, "مؤقت", "Transient")); }
            Assert.Equal(101, bindings.Count);
        }
        Assert.Equal(1, bindings.Count);
        foreach (var item in bindings) item.Set(item.Ar);
        Assert.Equal("عربي", permanent.Text);
        foreach (var item in bindings) item.Set(item.En);
        Assert.Equal("English", permanent.Text);
    }
    [Fact]
    public void Closed_dialog_control_can_be_collected_while_window_bindings_stay_alive()
    {
        var bindings = new TranslationBindings();
        var weak = CreateClosedDialog(bindings);
        for (var i = 0; i < 3 && weak.IsAlive; i++) { GC.Collect(); GC.WaitForPendingFinalizers(); GC.Collect(); }
        Assert.False(weak.IsAlive);
        Assert.Equal(0, bindings.Count);
        GC.KeepAlive(bindings);
    }
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static WeakReference CreateClosedDialog(TranslationBindings bindings)
    {
        using var scope = bindings.BeginScope();
        var label = new Label(); bindings.Add((value => label.Text = value, "مؤقت", "Transient"));
        return new WeakReference(label);
    }
}
