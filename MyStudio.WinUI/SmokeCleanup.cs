namespace MyStudio.WinUI;

internal static class SmokeCleanup
{
    internal static async Task<IReadOnlyList<Exception>> RunAsync(IEnumerable<Func<Task>> cleanup, Func<Task>? shutdown = null)
    {
        var errors = new List<Exception>();
        try
        {
            foreach (var operation in cleanup)
                try { await operation(); } catch (Exception error) { errors.Add(error); }
        }
        finally { if (shutdown is not null) await shutdown(); }
        return errors;
    }
}
