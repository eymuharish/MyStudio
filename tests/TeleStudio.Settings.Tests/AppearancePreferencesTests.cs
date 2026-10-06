using TeleStudio.Settings;
namespace TeleStudio.Settings.Tests;
public sealed class AppearancePreferencesTests
{
    [Theory]
    [InlineData(null, "en", "System")]
    [InlineData("{broken", "en", "System")]
    [InlineData("{}", "en", "System")]
    [InlineData("{\"language\":\"unknown\",\"theme\":\"Bad\"}", "en", "System")]
    [InlineData("{\"language\":\"ar\",\"theme\":\"Dark\"}", "ar", "Dark")]
    public void Appearance_defaults_to_English_and_preserves_saved_Arabic(string? json, string language, string theme)
        => Assert.Equal(new AppearancePreferences(language, theme), AppearancePreferences.Parse(json));
    [Fact]
    public void Existing_profile_Arabic_is_preserved_without_an_appearance_file()
        => Assert.Equal("ar", AppearancePreferences.Parse(null, "ar").Language);
    [Theory]
    [InlineData("10.0.0.1", true)]
    [InlineData("sip:doctor@example.org", true)]
    [InlineData("sip:user:password@example.org", false)]
    [InlineData("10.0.0.1?token=secret", false)]
    public void Recent_destinations_exclude_embedded_credentials(string address, bool expected)
        => Assert.Equal(expected, RecentDestinationPolicy.IsSafe(address));
}
