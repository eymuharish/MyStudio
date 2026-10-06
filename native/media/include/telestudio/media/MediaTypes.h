#pragma once

#include <optional>

namespace telestudio {

enum class Protocol {
    Sip,
    H323,
};

enum class VideoCodec {
    H264,
    H265,
};

enum class VideoSource {
    Camera,
    Desktop,
};

enum class EndpointModel {
    Auto = 0,
    Sx20 = 1,
    PcsXg77 = 2,
    PcsXg80 = 5,
    Generic = 6,
};

enum class QualityMode {
    Auto,
    Presentation,
    Motion,
    Maximum,
};

enum class VideoResolution { Auto, Sd, Hd, Fhd };

struct VideoPreferences {
    VideoResolution resolution = VideoResolution::Auto;
    std::optional<int> bitrateKbps;
};

struct RemoteCapabilities {
    bool h264Baseline;
    bool h264High;
    bool h265;
    bool fps60;
    int maxWidth;
    int maxHeight;
    int maxBitrateKbps;
};

struct MediaFormat {
    VideoCodec codec;
    int width;
    int height;
    int fps;
    int bitrateKbps;
    bool highProfile;
};

struct NetworkSample {
    double lossPercent;
    double jitterMs;
    double oneWayLatencyMs;
    int availableKbps;
    bool encoderOverloaded;
    int repeatedNacks;
};

} // namespace telestudio
