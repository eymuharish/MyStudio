using TeleStudio.Contracts;

namespace TeleStudio.Desktop.Tests;

public sealed class OpalStudioSupportTests
{
    [Fact]
    public void Aac_offer_keeps_stereo_and_mono_choices_when_only_g722_was_observed()
    {
        var catalog = Catalog(new OpalStudioProtocol(CallProtocol.Sip, [G722()]));
        Assert.True(catalog.Allows("Studio34", CallProtocol.Sip, true));
        Assert.Contains(new AudioFormatOption(2, 192), catalog.AllowedFormats("Studio34", CallProtocol.Sip, EngineFamily.Sony, true));
        Assert.Contains(new AudioFormatOption(1, 96), catalog.AllowedFormats("Studio34", CallProtocol.Sip, EngineFamily.Sony, true));
        Assert.Equal((CallProtocol.Sip, true), catalog.Recommendation(catalog.Studios[0]));
        Assert.NotNull(catalog.RequireAllowed("Studio34", CallProtocol.Sip, true, 2, 192));
    }
    private static OpalStudioCatalog PackagedCatalog()
    {
        var path = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "..", "..", "..", "..", "MyStudio.WinUI", "Assets", "opal-supported-studios.json"));
        return OpalStudioCatalog.Parse(File.ReadAllText(path));
    }
    private static OpalStudioCatalog Catalog(params OpalStudioProtocol[] protocols) => new(1,
        [new("034", "10.136.51.11", ["Studio34"], EndpointProfile.SonyXg77, CallProtocol.H323, "Sony XG77", "Sony XG77", protocols)]);
    private static OpalStudioAudioMode G722(string quality = "") => new("g722", "g722", "g722", true, quality, quality);

    [Fact]
    public void Quiet_microphone_is_a_caveat_and_does_not_disable_stable_g722()
    {
        var catalog = Catalog(new OpalStudioProtocol(CallProtocol.H323, [G722("Microphone near silence")]));
        Assert.True(catalog.Allows("Studio34", CallProtocol.H323, false));
        Assert.Contains("Microphone near silence", catalog.Notes("10.136.51.11", false));
        Assert.Equal((CallProtocol.H323, true), catalog.Recommendation(catalog.Studios[0]));
    }
    [Fact]
    public void Sip_aac_preference_allows_g722_fallback_without_claiming_observed_aac()
    {
        var catalog = Catalog(new OpalStudioProtocol(CallProtocol.Sip, [G722(), new("aac-lc", "g722", "g722", true)]));
        Assert.True(catalog.Allows("sip:user@10.136.51.11:5060", CallProtocol.Sip, false));
        Assert.True(catalog.Allows("10.136.51.11", CallProtocol.Sip, true));
        Assert.DoesNotContain("AAC-LC", catalog.Notes("Studio34", false));
        Assert.NotNull(catalog.RequireAllowed("10.136.51.11", CallProtocol.Sip, true));
    }
    [Fact]
    public void Stable_asymmetric_h323_aac_pair_is_allowed_and_notes_report_actual_directions()
    {
        var catalog = Catalog(new OpalStudioProtocol(CallProtocol.H323, [G722(), new("aac-lc", "aac-lc", "g722", true, Channels: 1, Kbps: 96)]));
        Assert.True(catalog.Allows("10.136.51.11", CallProtocol.H323, true));
        Assert.Contains("TX AAC-LC / RX G722", catalog.Notes("Studio34", false));
        Assert.Equal((CallProtocol.H323, true), catalog.Recommendation(catalog.Studios[0]));
        Assert.Equal(new[] { CallProtocol.H323 }, catalog.AllowedProtocols("Studio34"));
    }
    [Fact]
    public void Pending_or_unstable_known_connections_are_blocked_but_unknown_addresses_remain_manual()
    {
        var catalog = Catalog(new OpalStudioProtocol(CallProtocol.Sip, [G722() with { Stable = false }]));
        Assert.Empty(catalog.AllowedProtocols("10.136.51.11"));
        Assert.Null(catalog.Recommendation(catalog.Studios[0]));
        Assert.Throws<InvalidOperationException>(() => catalog.RequireAllowed("Studio34", CallProtocol.Sip, false));
        Assert.Contains("يلزم إعادة الاختبار", catalog.Notes("Studio34", true));
        Assert.True(catalog.Allows("192.0.2.22", CallProtocol.Sip, true));
        Assert.Equal(Enum.GetValues<CallProtocol>(), catalog.AllowedProtocols("192.0.2.22"));
        Assert.Contains("Untested", catalog.Notes("192.0.2.22", false));
    }
    [Fact]
    public void Packaged_identity_catalog_corrects_studio018_without_altering_the_legacy_directory_seed()
    {
        var catalog = PackagedCatalog();
        Assert.Equal(EndpointProfile.SonyXg80, catalog.Find("Studio018")!.EndpointProfile);
        Assert.Equal("10.136.61.31", catalog.Find("Studio018")!.Address);
        Assert.Equal(EndpointProfile.SonyXg77, catalog.Find("Studio34")!.EndpointProfile);
        Assert.Equal(6, catalog.Studios.Length);
        Assert.Throws<System.Text.Json.JsonException>(() => OpalStudioCatalog.Parse("{\"schemaVersion\":2,\"studios\":[]}"));
    }
    [Fact]
    public async Task Unsupported_known_protocol_is_rejected_before_any_native_dial_preflight()
    {
        var worker = new SharedOpalConsoleTests.Worker();
        await using var engine = new OpalEngineCommandSink(new SharedOpalConsole(worker), EngineFamily.Sony)
        { StudioSupport = Catalog(new OpalStudioProtocol(CallProtocol.H323, [G722()])) };
        await engine.StartAsync(default); worker.Commands.Clear();
        await Assert.ThrowsAsync<InvalidOperationException>(() => engine.SendAsync(new Dial("10.136.51.11", CallProtocol.Sip, "MyStudio OPAL"), default));
        Assert.Empty(worker.Commands);
        await engine.SendAsync(new Dial("192.0.2.22", CallProtocol.Sip, "MyStudio OPAL"), default);
        Assert.Contains("call 192.0.2.22", worker.Commands);
    }
    [Fact]
    public void Studio_directory_overlay_corrects_known_models_and_preserves_custom_entries()
    {
        var catalog = Catalog();
        var custom = new TeleStudio.Settings.RoomDirectoryEntry("custom", "My room", "192.0.2.55", EndpointProfile.Generic);
        var old = new TeleStudio.Settings.RoomDirectoryEntry("original", "Studio34", "10.136.51.11", EndpointProfile.SonyXg80);
        var updated = catalog.ApplyDirectory([old, custom]);
        Assert.Equal(EndpointProfile.SonyXg77, updated[0].EndpointProfile);
        Assert.Equal(old with { EndpointProfile = EndpointProfile.SonyXg77 }, updated[0]);
        Assert.Equal(custom, updated[1]);
        Assert.Equal(EndpointProfile.SonyXg80, old.EndpointProfile);
        Assert.Equal("10.136.51.11", Assert.Single(catalog.ApplyDirectory([])).Destination);
    }
    [Fact]
    public async Task Deleted_studios_remain_absent_after_saving_and_reloading()
    {
        var folder = Directory.CreateTempSubdirectory("MyStudio deleted rooms ");
        var file = Path.Combine(folder.FullName, "rooms.json");
        var catalog = Catalog();
        Assert.Single(catalog.ApplyDirectory([]));
        await new TeleStudio.Settings.RoomDirectoryStore(file).SaveAsync([], default);
        var saved = await new TeleStudio.Settings.RoomDirectoryStore(file).LoadAsync(default);
        Assert.Empty(catalog.ApplyDirectory(saved, addMissing: false));
        Assert.NotNull(catalog.Find("10.136.51.11"));
    }

    [Fact]
    public void Cisco_aac_offers_stereo_and_mono_with_observed_protocol_guard()
    {
        var catalog = new OpalStudioCatalog(1, [new("047", "10.136.51.25", ["SX20"], EndpointProfile.Sx20, CallProtocol.Sip, "", "",
            [new(CallProtocol.Sip, [G722(), new("aac-ld", "aac-ld", "aac-ld", true, Channels: 1, Kbps: 64)])])]);
        Assert.Equal(new[] { new AudioFormatOption(2, 128), new AudioFormatOption(1, 64) }, catalog.AllowedFormats("SX20", CallProtocol.Sip, EngineFamily.Cisco, true));
        Assert.NotNull(catalog.RequireAllowed("SX20", CallProtocol.Sip, true, 2, 128));
        Assert.Equal((CallProtocol.Sip, true), catalog.Recommendation(catalog.Studios[0]));
        Assert.Equal(new[] { new AudioFormatOption(2, 128), new AudioFormatOption(1, 64) },
            catalog.AllowedFormats("192.0.2.22", CallProtocol.H323, EngineFamily.Cisco, true));
    }
    [Fact]
    public async Task Known_name_dials_canonical_address_only_after_correct_model_preflight()
    {
        var worker = new SharedOpalConsoleTests.Worker();
        await using var engine = new OpalEngineCommandSink(new SharedOpalConsole(worker), EngineFamily.Sony)
        { StudioSupport = Catalog(new OpalStudioProtocol(CallProtocol.H323, [G722()])) };
        await engine.StartAsync(default); worker.Commands.Clear();
        await Assert.ThrowsAsync<InvalidOperationException>(() => engine.SendAsync(new Dial("Studio34", CallProtocol.H323, "MyStudio OPAL"), default));
        Assert.Empty(worker.Commands);
        await engine.SendAsync(new SetMediaPreferences(CodecPreference.H264, QualityMode.Auto, EndpointProfile.SonyXg77), default);
        await engine.SendAsync(new SetNetworkMode(NetworkConnectionMode.ExternalVpn), default);
        await engine.SendAsync(new SetVideoPreferences(VideoResolution.Fhd, 4000), default);
        await engine.SendAsync(new Dial("Studio34", CallProtocol.H323, "MyStudio OPAL"), default);
        Assert.Contains("call 10.136.51.11", worker.Commands);
        Assert.DoesNotContain(worker.Commands, command => command.Contains("Studio34", StringComparison.Ordinal));
        Assert.Contains("network-mode external", worker.Commands);
        Assert.Contains("video-preferences fhd 4000", worker.Commands);
    }
    [Fact]
    public async Task Known_g722_baseline_allows_aac_preference_with_plugin_loaded()
    {
        var worker = new SharedOpalConsoleTests.Worker();
        await using var engine = new OpalEngineCommandSink(new SharedOpalConsole(worker), EngineFamily.Sony)
        { StudioSupport = Catalog(new OpalStudioProtocol(CallProtocol.Sip, [G722(), new("aac-lc", "g722", "g722", true, Channels: 2, Kbps: 192)])) };
        await engine.StartAsync(default);
        worker.Emit("{\"schema\":1,\"type\":\"aac-availability\",\"sequence\":1,\"protocol\":\"sip\",\"available\":true,\"detail\":\"loaded\"}");
        await engine.SendAsync(new SetMediaPreferences(CodecPreference.H264, QualityMode.Auto, EndpointProfile.SonyXg77), default);
        await engine.SendAsync(new SetConnectionTuning(new ConnectionTuning { AacEnabled = true }), default);
        worker.Commands.Clear();
        await engine.SendAsync(new Dial("Studio34", CallProtocol.Sip, "MyStudio OPAL"), default);
        Assert.Contains("call 10.136.51.11", worker.Commands);
    }
    [Fact]
    public void Final_observed_device_policy_matches_negotiated_codecs_and_readable_arabic_notes()
    {
        var catalog = PackagedCatalog();
        Assert.True(catalog.Allows("032", CallProtocol.Sip, true));
        Assert.True(catalog.Allows("032", CallProtocol.H323, true));
        Assert.Equal(new[] { CallProtocol.H323 }, catalog.AllowedProtocols("018"));
        Assert.False(catalog.Allows("sips:10.136.61.31:5061", CallProtocol.Sip, false));
        Assert.False(catalog.Allows("sips:user@10.136.61.31:5061", CallProtocol.Sip, false));
        Assert.True(catalog.Allows("018", CallProtocol.H323, true));
        Assert.Equal((CallProtocol.H323, true), catalog.Recommendation(catalog.Find("018")!));
        Assert.Contains("nearly silent", catalog.Notes("034", false));
        Assert.Contains("شبه صامت", catalog.Notes("034", true));
        Assert.True(catalog.Allows("034", CallProtocol.Sip, false));
        Assert.True(catalog.Allows("034", CallProtocol.H323, true));
        Assert.Equal(new[] { new AudioFormatOption(2, 192), new AudioFormatOption(1, 96) }, catalog.AllowedFormats("034", CallProtocol.H323, EngineFamily.Sony, true));
        foreach (var studio in catalog.Studios)
        {
            Assert.DoesNotContain("ط§", studio.NotesAr);
            Assert.DoesNotContain("ظ„", studio.NotesAr);
            Assert.DoesNotContain("§", studio.NotesAr);
        }
        foreach (var studio in catalog.Studios.Where(x => x.Family == EngineFamily.Cisco))
        {
            Assert.Equal(new[] { new AudioFormatOption(2, 128), new AudioFormatOption(1, 64) }, catalog.AllowedFormats(studio.Address, CallProtocol.Sip, EngineFamily.Cisco, true));
            Assert.NotNull(catalog.RequireAllowed(studio.Address, CallProtocol.Sip, true, 2, 128));
        }
    }
    [Theory]
    [InlineData("sip:10.136.51.11")]
    [InlineData("sips:user@10.136.51.11:5061")]
    [InlineData("sips:Studio34")]
    [InlineData("#h323:@10.136.51.11")]
    public async Task Known_prefixed_destinations_cannot_bypass_policy_and_use_verified_protocol_address(string destination)
    {
        var worker = new SharedOpalConsoleTests.Worker();
        await using var engine = new OpalEngineCommandSink(new SharedOpalConsole(worker), EngineFamily.Sony)
        { StudioSupport = Catalog(new OpalStudioProtocol(CallProtocol.H323, [G722()])) };
        await engine.StartAsync(default); worker.Commands.Clear();
        await Assert.ThrowsAsync<InvalidOperationException>(() => engine.SendAsync(new Dial(destination, CallProtocol.Sip, "MyStudio OPAL"), default));
        Assert.Empty(worker.Commands);
        await engine.SendAsync(new SetMediaPreferences(CodecPreference.H264, QualityMode.Auto, EndpointProfile.SonyXg77), default);
        await engine.SendAsync(new Dial(destination, CallProtocol.H323, "MyStudio OPAL"), default);
        Assert.Equal("call 10.136.51.11", worker.Commands.Last());
    }
}

