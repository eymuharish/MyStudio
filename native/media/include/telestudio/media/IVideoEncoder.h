#pragma once

#include "telestudio/media/MediaTypes.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace telestudio {

struct I420Frame {
    const uint8_t* y;
    const uint8_t* u;
    const uint8_t* v;
    int strideY;
    int strideU;
    int strideV;
    int width;
    int height;
    int64_t timestamp100ns;
};

struct EncoderSettings {
    VideoCodec codec;
    int width;
    int height;
    int fps;
    int bitrateKbps;
    bool highProfile;
    int keyintFrames;
};

struct EncodedAccessUnit {
    std::vector<uint8_t> annexB;
    int64_t timestamp100ns;
    bool keyFrame;
};

class IVideoEncoder {
public:
    virtual ~IVideoEncoder() = default;
    virtual void Open(const EncoderSettings& settings) = 0;
    virtual EncodedAccessUnit Encode(const I420Frame& frame, bool forceIdr) = 0;
    virtual void Reconfigure(const EncoderSettings& settings) = 0;
};

std::unique_ptr<IVideoEncoder> CreateX264Encoder(const std::filesystem::path& dll);
std::unique_ptr<IVideoEncoder> CreateX265Encoder(const std::filesystem::path& dll);

} // namespace telestudio
