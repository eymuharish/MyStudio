using System.Text;

namespace TeleStudio.Contracts;

public static class VideoCommandCodec
{
    private static readonly UTF8Encoding StrictUtf8 = new(false, true);
    public static string Format(SetVideoPreferences value) => $"video-preferences {ResolutionToken(value.Resolution)} {value.BitrateKbps?.ToString(System.Globalization.CultureInfo.InvariantCulture) ?? "auto"}";
    public static string Format(SetCameraEnabled value) => value.Enabled ? value.DeviceId is null ? "camera on" : $"camera on {EncodeIdentifier(value.DeviceId, nameof(value.DeviceId))}" : RequireNoIdentifier(value.DeviceId, "camera off");
    public static string Format(SetDesktopEnabled value) => value.Enabled ? $"desktop on {EncodeIdentifier(value.DisplayId ?? throw new ArgumentException("Desktop identifier is required.", nameof(value.DisplayId)), nameof(value.DisplayId))}" : RequireNoIdentifier(value.DisplayId, "desktop off");
    public static string Format(SetCameraOverlay value) => $"camera-overlay {SizeToken(value.Size)} {PositionToken(value.Position)}";
    public static string ResolutionToken(VideoResolution value) => value switch { VideoResolution.Auto => "auto", VideoResolution.Sd => "sd", VideoResolution.Hd => "hd", VideoResolution.Fhd => "fhd", _ => throw new ArgumentOutOfRangeException(nameof(value)) };
    public static string SizeToken(CameraOverlaySize value) => value switch { CameraOverlaySize.Small => "small", CameraOverlaySize.Medium => "medium", CameraOverlaySize.Large => "large", _ => throw new ArgumentOutOfRangeException(nameof(value)) };
    public static string PositionToken(CameraOverlayPosition value) => value switch { CameraOverlayPosition.TopLeft => "top-left", CameraOverlayPosition.TopRight => "top-right", CameraOverlayPosition.BottomLeft => "bottom-left", CameraOverlayPosition.BottomRight => "bottom-right", _ => throw new ArgumentOutOfRangeException(nameof(value)) };
    private static string RequireNoIdentifier(string? identifier, string command) => identifier is null ? command : throw new ArgumentException("An identifier is not allowed when disabled.", nameof(identifier));
    private static string EncodeIdentifier(string identifier, string parameterName)
    {
        byte[] bytes;
        try { bytes = StrictUtf8.GetBytes(identifier); } catch (EncoderFallbackException error) { throw new ArgumentException("Identifier is not valid UTF-8 text.", parameterName, error); }
        if (bytes.Length is < 1 or > 2048 || identifier.Any(value => value is <= '\u001f' or >= '\u007f' and <= '\u009f')) throw new ArgumentOutOfRangeException(parameterName);
        return Convert.ToBase64String(bytes).TrimEnd('=').Replace('+', '-').Replace('/', '_');
    }
}
