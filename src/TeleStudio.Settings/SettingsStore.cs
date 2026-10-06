using System.Text.Json;
using System.Text.Json.Serialization;
using TeleStudio.Contracts;

namespace TeleStudio.Settings;

public sealed class SettingsStore
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        WriteIndented = true,
        Converters =
        {
            new CodecPreferenceConverter(),
            new QualityModeConverter(),
            new VideoResolutionConverter(),
            new CameraOverlaySizeConverter(),
            new CameraOverlayPositionConverter(),
            new EndpointProfileConverter(),
            new NetworkConnectionModeConverter(),
            new SipEngineKindConverter(),
        }
    };

    private readonly string _path;

    public SettingsStore(string path)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        _path = Path.GetFullPath(path);
    }

    public async Task<AppSettings> LoadAsync(CancellationToken cancellationToken)
    {
        if (!File.Exists(_path))
        {
            return AppSettings.Default;
        }

        try
        {
            await using var stream = new FileStream(
                _path,
                FileMode.Open,
                FileAccess.Read,
                FileShare.Read,
                bufferSize: 16 * 1024,
                FileOptions.Asynchronous | FileOptions.SequentialScan);
            var settings = await JsonSerializer.DeserializeAsync<AppSettings>(stream, JsonOptions, cancellationToken)
                ?? throw new JsonException("Settings document is empty");
            Validate(settings);
            return settings;
        }
        catch (Exception error) when (error is JsonException or NotSupportedException or InvalidDataException)
        {
            PreserveCorruptFile();
            return AppSettings.Default;
        }
    }

    public async Task SaveAsync(AppSettings settings, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(settings);
        Validate(settings);

        var directory = Path.GetDirectoryName(_path) ?? throw new InvalidOperationException("Settings path requires a directory");
        Directory.CreateDirectory(directory);
        var temporaryPath = Path.Combine(directory, $".{Path.GetFileName(_path)}.{Guid.NewGuid():N}.tmp");

        try
        {
            await using (var stream = new FileStream(
                temporaryPath,
                FileMode.CreateNew,
                FileAccess.Write,
                FileShare.None,
                bufferSize: 16 * 1024,
                FileOptions.Asynchronous | FileOptions.WriteThrough))
            {
                await JsonSerializer.SerializeAsync(stream, settings, JsonOptions, cancellationToken);
                await stream.FlushAsync(cancellationToken);
            }
            File.Move(temporaryPath, _path, overwrite: true);
        }
        finally
        {
            File.Delete(temporaryPath);
        }
    }

    private static void Validate(AppSettings settings)
    {
        if (settings.Language is not ("ar" or "en"))
        {
            throw new InvalidDataException("Unsupported language");
        }
        if (settings.RtpPortStart is < 1024 or > 65535 || settings.RtpPortEnd < settings.RtpPortStart || settings.RtpPortEnd > 65535)
        {
            throw new InvalidDataException("Invalid RTP port range");
        }
        if (settings.RecentDestinations is null)
        {
            throw new InvalidDataException("Recent destinations are required");
        }
        if (settings.RecentDestinations.Count > 10)
        {
            throw new InvalidDataException("Too many recent destinations");
        }
        if (!Enum.IsDefined(settings.SipEngine) || !Enum.IsDefined(settings.Codec) || !Enum.IsDefined(settings.Quality) || !Enum.IsDefined(settings.Endpoint) || !Enum.IsDefined(settings.NetworkMode) || !Enum.IsDefined(settings.VideoResolution) || !Enum.IsDefined(settings.CameraOverlaySize) || !Enum.IsDefined(settings.CameraOverlayPosition) || settings.VideoBitrateKbps is < 128 or > 20000)
        {
            throw new InvalidDataException("Invalid media preference");
        }
        try
        {
            if (!string.Equals(settings.LecturerName, LecturerNameCodec.Normalize(settings.LecturerName), StringComparison.Ordinal))
            {
                throw new InvalidDataException("Lecturer name must be normalized");
            }
        }
        catch (ArgumentException error)
        {
            throw new InvalidDataException("Invalid lecturer name", error);
        }
        if (settings.AudioDscp is < 0 or > 63 || settings.VideoDscp is < 0 or > 63 || settings.SignalingDscp is < 0 or > 63)
        {
            throw new InvalidDataException("Invalid DSCP value");
        }
        if (settings.RoomVolume is < 0 or > 100 || settings.MicrophoneGain is < 0 or > 150)
        {
            throw new InvalidDataException("Invalid audio control value");
        }
    }

    private void PreserveCorruptFile()
    {
        var directory = Path.GetDirectoryName(_path)!;
        var baseName = Path.GetFileNameWithoutExtension(_path);
        var extension = Path.GetExtension(_path);
        var timestamp = DateTimeOffset.UtcNow.ToString("yyyyMMddHHmmssfff");
        var backupPath = Path.Combine(directory, $"{baseName}.corrupt-{timestamp}-{Guid.NewGuid():N}{extension}");
        File.Move(_path, backupPath);
    }

    private abstract class ExactEnumConverter<T> : JsonConverter<T> where T : struct, Enum
    {
        private readonly IReadOnlyDictionary<string, T> _fromToken;
        private readonly IReadOnlyDictionary<T, string> _toToken;

        protected ExactEnumConverter(params (string Token, T Value)[] values)
        {
            _fromToken = values.ToDictionary(value => value.Token, value => value.Value, StringComparer.Ordinal);
            _toToken = values.ToDictionary(value => value.Value, value => value.Token);
        }

        public override T Read(ref Utf8JsonReader reader, Type typeToConvert, JsonSerializerOptions options)
        {
            if (reader.TokenType != JsonTokenType.String || !_fromToken.TryGetValue(reader.GetString()!, out var value))
            {
                throw new JsonException("Invalid enum token");
            }
            return value;
        }

        public override void Write(Utf8JsonWriter writer, T value, JsonSerializerOptions options)
        {
            if (!_toToken.TryGetValue(value, out var token))
            {
                throw new JsonException("Invalid enum value");
            }
            writer.WriteStringValue(token);
        }
    }

    private sealed class CodecPreferenceConverter() : ExactEnumConverter<CodecPreference>(
        ("auto", CodecPreference.Auto), ("h264", CodecPreference.H264), ("h265", CodecPreference.H265));

    private sealed class QualityModeConverter() : ExactEnumConverter<QualityMode>(
        ("auto", QualityMode.Auto), ("presentation", QualityMode.Presentation),
        ("motion", QualityMode.Motion), ("maximum", QualityMode.Maximum));
    private sealed class VideoResolutionConverter() : ExactEnumConverter<VideoResolution>(("auto", VideoResolution.Auto), ("sd", VideoResolution.Sd), ("hd", VideoResolution.Hd), ("fhd", VideoResolution.Fhd));
    private sealed class CameraOverlaySizeConverter() : ExactEnumConverter<CameraOverlaySize>(("small", CameraOverlaySize.Small), ("medium", CameraOverlaySize.Medium), ("large", CameraOverlaySize.Large));
    private sealed class CameraOverlayPositionConverter() : ExactEnumConverter<CameraOverlayPosition>(("top-left", CameraOverlayPosition.TopLeft), ("top-right", CameraOverlayPosition.TopRight), ("bottom-left", CameraOverlayPosition.BottomLeft), ("bottom-right", CameraOverlayPosition.BottomRight));

    private sealed class EndpointProfileConverter() : ExactEnumConverter<EndpointProfile>(
        ("auto", EndpointProfile.Auto), ("sx20", EndpointProfile.Sx20),
        ("ex90", EndpointProfile.Ex90),
        ("pcs-xg77", EndpointProfile.SonyXg77), ("pcs-xg80", EndpointProfile.SonyXg80),
        ("generic", EndpointProfile.Generic));

    private sealed class NetworkConnectionModeConverter : JsonConverter<NetworkConnectionMode>
    {
        public override NetworkConnectionMode Read(ref Utf8JsonReader reader, Type type, JsonSerializerOptions options)
        {
            if (reader.TokenType == JsonTokenType.String && reader.GetString()?.ToLowerInvariant() is "building" or "external-vpn") return NetworkConnectionMode.ExternalVpn;
            if (reader.TokenType == JsonTokenType.Number && reader.TryGetInt32(out var value) && value is 0 or 1) return NetworkConnectionMode.ExternalVpn;
            throw new JsonException("Unknown network mode");
        }
        public override void Write(Utf8JsonWriter writer, NetworkConnectionMode value, JsonSerializerOptions options)
        {
            if (!Enum.IsDefined(value)) throw new JsonException("Unknown network mode");
            writer.WriteStringValue("external-vpn");
        }
    }

    private sealed class SipEngineKindConverter : JsonConverter<SipEngineKind>
    {
        public override SipEngineKind Read(ref Utf8JsonReader reader, Type type, JsonSerializerOptions options)
        {
            // Read retired backend identifiers solely to migrate existing preference documents.
            if (reader.TokenType == JsonTokenType.String && reader.GetString()?.ToLowerInvariant() is "opal" or "pjsip" or "liblinphone") return SipEngineKind.Opal;
            if (reader.TokenType == JsonTokenType.Number && reader.TryGetInt32(out var value) && value is 0 or 1 or 2) return SipEngineKind.Opal;
            throw new JsonException("Invalid SIP engine token");
        }
        public override void Write(Utf8JsonWriter writer, SipEngineKind value, JsonSerializerOptions options)
        {
            if (value != SipEngineKind.Opal) throw new JsonException("This edition uses OPAL only");
            writer.WriteStringValue("opal");
        }
    }
}
