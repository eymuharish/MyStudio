#pragma once

#include "telestudio/media/IVideoEncoder.h"

#include <cstdint>
#include <vector>

namespace telestudio {

enum class CameraOverlaySize { Small, Medium, Large };
enum class CameraOverlayPosition { TopLeft, TopRight, BottomLeft, BottomRight };

struct VideoCompositionSettings {
    int width = 1280;
    int height = 720;
    CameraOverlaySize cameraSize = CameraOverlaySize::Medium;
    CameraOverlayPosition cameraPosition = CameraOverlayPosition::BottomRight;
};

struct ComposedVideoFrame {
    std::vector<uint8_t> y;
    std::vector<uint8_t> u;
    std::vector<uint8_t> v;
    int width = 0;
    int height = 0;
    int64_t timestamp100ns = 0;

    [[nodiscard]] I420Frame View() const noexcept;
};

[[nodiscard]] ComposedVideoFrame ComposeVideoFrame(const I420Frame* desktop,
                                                    const I420Frame* camera,
                                                    const VideoCompositionSettings& settings,
                                                    int64_t timestamp100ns);

} // namespace telestudio
