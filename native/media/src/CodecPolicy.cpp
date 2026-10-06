#include "telestudio/media/CodecPolicy.h"

#include <algorithm>
#include <stdexcept>

namespace telestudio {
namespace {

struct RequestedFormat {
    int width;
    int height;
    int fps;
    int bitrateKbps;
};

bool IsSony(EndpointModel endpoint) {
    return endpoint == EndpointModel::PcsXg77 || endpoint == EndpointModel::PcsXg80;
}

bool AllowsH265Fallback(EndpointModel endpoint) {
    return endpoint == EndpointModel::Auto || endpoint == EndpointModel::Generic;
}

RequestedFormat SonyRequestedFormat(QualityMode quality, const RemoteCapabilities& capabilities) {
    switch (quality) {
    case QualityMode::Presentation:
        return {1920, 1080, 15, 2000};
    case QualityMode::Motion:
        return {1280, 720, capabilities.fps60 ? 60 : 30, 2048};
    case QualityMode::Maximum:
        return {1920, 1080, capabilities.fps60 ? 60 : 30, capabilities.maxBitrateKbps};
    case QualityMode::Auto:
        return {1280, 720, 30, 2048};
    }
    return {1280, 720, 30, 2048};
}

RequestedFormat ResolveRequestedFormat(EndpointModel endpoint,
                                       Protocol protocol,
                                       VideoSource source,
                                       QualityMode quality,
                                       const RemoteCapabilities& capabilities) {
    if (endpoint == EndpointModel::Sx20) {
        if (source == VideoSource::Desktop) {
            return {1920, 1080, 15, 4000};
        }
        if (quality == QualityMode::Motion) {
            return {1280, 720, capabilities.fps60 ? 60 : 30, 4000};
        }
        if (quality == QualityMode::Maximum) {
            return {1920, 1080, capabilities.fps60 ? 60 : 30, 6000};
        }
        return {1920, 1080, 30, 2500};
    }

    if (IsSony(endpoint)) {
        return SonyRequestedFormat(quality, capabilities);
    }

    return {1280, 720, 30, 2000};
}

void ClampResolution(RequestedFormat& requested, const RemoteCapabilities& capabilities) {
    if (requested.width >= 1920 && requested.height >= 1080 &&
        capabilities.maxWidth >= 1920 && capabilities.maxHeight >= 1080) {
        requested.width = 1920;
        requested.height = 1080;
        return;
    }
    if (requested.width >= 1280 && requested.height >= 720 &&
        capabilities.maxWidth >= 1280 && capabilities.maxHeight >= 720) {
        requested.width = 1280;
        requested.height = 720;
        return;
    }
    if (requested.width >= 1024 && requested.height >= 576 &&
        capabilities.maxWidth >= 1024 && capabilities.maxHeight >= 576) {
        requested.width = 1024;
        requested.height = 576;
        return;
    }
    if (requested.width >= 640 && requested.height >= 480 &&
        capabilities.maxWidth >= 640 && capabilities.maxHeight >= 480) {
        requested.width = 640;
        requested.height = 480;
        return;
    }
    throw std::invalid_argument("remote capabilities do not support a standard video rung");
}

void ApplyPreferences(RequestedFormat& requested, const VideoPreferences& preferences) {
    switch (preferences.resolution) {
    case VideoResolution::Auto: break;
    case VideoResolution::Sd: requested.width = 640; requested.height = 480; break;
    case VideoResolution::Hd: requested.width = 1280; requested.height = 720; break;
    case VideoResolution::Fhd: requested.width = 1920; requested.height = 1080; break;
    default: throw std::invalid_argument("invalid video resolution preference");
    }
    if (preferences.bitrateKbps) {
        if (*preferences.bitrateKbps < 128 || *preferences.bitrateKbps > 20000) throw std::invalid_argument("invalid video bitrate preference");
        requested.bitrateKbps = *preferences.bitrateKbps;
    }
}

} // namespace

MediaFormat ResolveMediaFormat(EndpointModel endpoint,
                               Protocol protocol,
                               VideoSource source,
                               QualityMode quality,
                               const RemoteCapabilities& capabilities,
                               const VideoPreferences& preferences) {
    const bool h264Available = capabilities.h264Baseline || capabilities.h264High;
    const bool h265Fallback = !h264Available && capabilities.h265 && protocol == Protocol::Sip &&
                              AllowsH265Fallback(endpoint);
    if (!h264Available && !h265Fallback) {
        throw std::invalid_argument("remote capabilities have no allowed video codec");
    }
    if (capabilities.maxBitrateKbps <= 0) {
        throw std::invalid_argument("remote bitrate capability must be positive");
    }

    RequestedFormat requested = ResolveRequestedFormat(endpoint, protocol, source, quality, capabilities);
    ApplyPreferences(requested, preferences);
    ClampResolution(requested, capabilities);
    requested.fps = std::min(requested.fps, capabilities.fps60 ? 60 : 30);
    if (IsSony(endpoint)) {
        requested.width = std::min(requested.width, 1280);
        requested.height = std::min(requested.height, 720);
        requested.fps = std::min(requested.fps, 30);
        requested.bitrateKbps = std::min(requested.bitrateKbps, 2048);
    }
    requested.bitrateKbps = std::min(requested.bitrateKbps, capabilities.maxBitrateKbps);

    return {h265Fallback ? VideoCodec::H265 : VideoCodec::H264,
            requested.width,
            requested.height,
            requested.fps,
            requested.bitrateKbps,
            !h265Fallback && capabilities.h264High};
}

} // namespace telestudio
