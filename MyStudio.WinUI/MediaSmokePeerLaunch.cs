using System.IO;

namespace MyStudio.WinUI;

internal sealed record MediaSmokePeerLaunch(string Executable, string Arguments)
{
    internal static MediaSmokePeerLaunch Create(string worker, string nativeArguments, string[] arguments)
    {
        var option = Array.IndexOf(arguments, "--peer-debugger");
        if (option < 0) return new(worker, nativeArguments);
        if (option + 1 >= arguments.Length || !Path.IsPathFullyQualified(arguments[option + 1]))
            throw new ArgumentException("--peer-debugger requires an absolute debugger executable path.");
        var debugger = arguments[option + 1];
        if (!File.Exists(debugger)) throw new FileNotFoundException("The peer debugger executable is missing.", debugger);
        if (!Path.IsPathFullyQualified(worker) || !File.Exists(worker)) throw new FileNotFoundException("The real OPAL peer worker is missing.", worker);
        // A verified Windows .exe filename cannot contain quotes or end in a backslash.
        return new(debugger, "--stacks-after-ms 35000 \"" + worker + "\" " + nativeArguments);
    }
}
