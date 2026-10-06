using System.Text.Json;
namespace TeleStudio.Settings;
public sealed record AppearancePreferences(string Language = "en", string Theme = "System")
{
    public static AppearancePreferences Parse(string? json, string fallbackLanguage = "en")
    {
        if (json is null) return new(fallbackLanguage == "ar" ? "ar" : "en");
        try
        {
            using var doc = JsonDocument.Parse(json); var root = doc.RootElement;
            var language = root.TryGetProperty("language", out var lang) && lang.ValueKind == JsonValueKind.String && lang.GetString() == "ar" ? "ar" : "en";
            var theme = root.TryGetProperty("theme", out var value) && value.ValueKind == JsonValueKind.String ? value.GetString() : "System";
            return new(language, theme is "Light" or "Dark" ? theme : "System");
        }
        catch (JsonException) { return new(); }
        catch (InvalidOperationException) { return new(); }
    }
}
