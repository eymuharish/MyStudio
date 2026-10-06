namespace MyStudio.WinUI;

/// <summary>Recording duration comes from the recorder, so pausing never pauses the call clock.</summary>
public readonly record struct CallBarTimers(string Call, string Recording)
{
    public static CallBarTimers Create(TimeSpan callElapsed, long recordingElapsedMs) =>
        new(Format(Math.Max(0, callElapsed.Ticks / TimeSpan.TicksPerSecond)), Format(Math.Max(0, recordingElapsedMs / 1000)));
    private static string Format(long seconds) => string.Create(System.Globalization.CultureInfo.InvariantCulture,
        $"{seconds / 3600:00}:{seconds / 60 % 60:00}:{seconds % 60:00}");
}

/// <summary>Confirmed state and capability availability remain separate.</summary>
public sealed record CallToolState(string Label, string Description, bool CrossedOut, bool Active)
{
    public static CallToolState Create(string name, string turnOff, string turnOn, bool off, bool available, bool arabic = false, bool? requested = null, string? error = null)
    {
        var label = !available ? name + (arabic ? " غير متاح" : " unavailable") : name + (off ? (arabic ? " متوقف" : " off") : (arabic ? " يعمل" : " on"));
        var description = !available
            ? label + (arabic ? ": غير متاح حتى تأكيد الاتصال والصلاحية" : ": Unavailable until the call and capability are confirmed")
            : label + ". " + (off ? turnOn : turnOff);
        if (requested is { } request)
        {
            if (request == off) label = name + (request ? (arabic ? " يبدأ" : " starting") : (arabic ? " يتوقف" : " stopping"));
            description = label + ". " + (request ? turnOff : turnOn);
            if (!string.IsNullOrEmpty(error)) description += ". " + error;
        }
        return new(label, description, off, (available || requested.HasValue) && !off);
    }
}

public sealed class CallBarVisibility
{
    private bool dismissed;
    public void Hide() => dismissed = true;
    public void Reopen() => dismissed = false;
    public bool ShouldShow(bool active)
    {
        if (!active) dismissed = false;
        return active && !dismissed;
    }
}

/// <summary>Physical window coordinates, including monitors left of the primary display.</summary>
public readonly record struct CallBarBounds(int X, int Y, int Width, int Height)
{
    public static CallBarBounds Clamp(int x, int y, int width, int height, int areaX, int areaY, int areaWidth, int areaHeight)
    {
        width = Math.Min(Math.Max(1, width), Math.Max(1, areaWidth));
        height = Math.Min(Math.Max(1, height), Math.Max(1, areaHeight));
        return new(Math.Clamp(x, areaX, areaX + Math.Max(0, areaWidth - width)),
            Math.Clamp(y, areaY, areaY + Math.Max(0, areaHeight - height)), width, height);
    }
    public static bool ShouldDrag(double dx, double dy) => Math.Abs(dx) >= 4 || Math.Abs(dy) >= 4;
}
