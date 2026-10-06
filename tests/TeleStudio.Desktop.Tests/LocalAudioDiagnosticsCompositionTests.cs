using TeleStudio.Desktop.Audio;

namespace TeleStudio.Desktop.Tests;

public sealed class LocalAudioDiagnosticsCompositionTests
{
    [Fact]
    public async Task Local_composition_constructs_only_WASAPI_diagnostics()
    {
        var composition = new LocalAudioDiagnosticsComposition();

        await using var diagnostics = composition.Create();

        Assert.IsType<WasapiAudioDiagnostics>(diagnostics);
        Assert.Empty(typeof(LocalAudioDiagnosticsComposition).GetFields(
            System.Reflection.BindingFlags.Instance |
            System.Reflection.BindingFlags.Static |
            System.Reflection.BindingFlags.Public |
            System.Reflection.BindingFlags.NonPublic));
        Assert.DoesNotContain(
            typeof(LocalAudioDiagnosticsComposition).GetConstructors()
                .SelectMany(constructor => constructor.GetParameters())
                .Select(parameter => parameter.ParameterType.FullName ?? string.Empty),
            name => name.Contains("Engine", StringComparison.OrdinalIgnoreCase) ||
                    name.Contains("Console", StringComparison.OrdinalIgnoreCase) ||
                    name.Contains("Process", StringComparison.OrdinalIgnoreCase));
    }
}
