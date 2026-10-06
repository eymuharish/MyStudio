namespace TeleStudio.Desktop;

public static class OpalCallLifecycle
{
    // OPAL's non-INVITE transaction timeout is 16 seconds. Allow the native
    // terminal notification and recording finalization to drain after it.
    public static readonly TimeSpan TerminationTimeout = TimeSpan.FromSeconds(25);
}
