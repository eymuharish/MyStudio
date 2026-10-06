using TeleStudio.Contracts;

namespace TeleStudio.Desktop;

/// <summary>Only known, non-fatal live tuning rejections may stay in the diagnostic log.</summary>
internal static class VideoTuningNoticePolicy
{
    public static bool IsLogOnly(MediaError error, bool connected) => connected &&
        ((error.Code == "video-preferences-failed" && error.Detail == "Encoder rejected bitrate change") ||
         (error.Code == "network-video-rejected" && error.Detail == "Encoder rejected network profile") ||
         (error.Code == "network-video-not-ready" && error.Detail == "Video negotiation is not yet ready."));
}
