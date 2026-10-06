using System.IO;
using Microsoft.UI.Xaml;
using TeleStudio.Settings;

namespace MyStudio.WinUI;

public partial class App : Application
{
    private Window? window;
    public App()
    {
        var parsed = AppDataPaths.ParseArguments(Environment.GetCommandLineArgs().Skip(1).ToArray());
        var args = Environment.GetCommandLineArgs();
        if (args.Contains("--studio-transition-smoke"))
        {
            TeleStudio.Desktop.StudioTransitionSmokePlan.RequireIsolation(parsed.Root, Path.Combine(AppContext.BaseDirectory, "OPALData"));
            if (args.Any(x => x is "--local-media-smoke" or "--ui-smoke" or "--call-bar-smoke"))
                throw new ArgumentException("Physical transition smoke runs separately from simulated/local UI modes.");
            Environment.SetEnvironmentVariable("ESSASTUDIO_TEST_MODE", "0");
            Environment.SetEnvironmentVariable("ESSASTUDIO_DIAGNOSTIC_LOG", "1");
        }
        var isolated = parsed.Root ?? Path.Combine(AppContext.BaseDirectory, "OPALData");
        Environment.SetEnvironmentVariable(AppDataPaths.EnvironmentVariable, isolated);
        Directory.CreateDirectory(isolated);
        if (Environment.GetCommandLineArgs().Contains("--local-media-smoke")) Environment.SetEnvironmentVariable("ESSASTUDIO_TEST_MODE", "1");
        InitializeComponent();
        UnhandledException += (_, error) => TeleStudio.Desktop.Runtime.DiagnosticLog.Write("unhandled", error.Exception.ToString());
    }
    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        window = new MainWindow(); window.Activate();
    }
}
