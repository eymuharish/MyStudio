#include "telestudio/media/VideoCompositor.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <stdexcept>

namespace telestudio {
namespace {

constexpr int kMaxWidth = 4096;
constexpr int kMaxHeight = 2160;

struct Rect { int x; int y; int width; int height; };

size_t CheckedSize(int width, int height) {
    if (width <= 0 || height <= 0 ||
        static_cast<size_t>(width) > std::numeric_limits<size_t>::max() / static_cast<size_t>(height)) {
        throw std::invalid_argument("I420 plane dimensions are invalid");
    }
    return static_cast<size_t>(width) * static_cast<size_t>(height);
}

void ValidateSettings(const VideoCompositionSettings& settings) {
    if (settings.width < 2 || settings.height < 2 || (settings.width & 1) || (settings.height & 1) ||
        settings.width > kMaxWidth || settings.height > kMaxHeight) {
        throw std::invalid_argument("composition canvas must be even and within 2..4096x2160");
    }
    if (settings.cameraSize != CameraOverlaySize::Small && settings.cameraSize != CameraOverlaySize::Medium &&
        settings.cameraSize != CameraOverlaySize::Large) {
        throw std::invalid_argument("camera overlay size is invalid");
    }
    if (settings.cameraPosition != CameraOverlayPosition::TopLeft &&
        settings.cameraPosition != CameraOverlayPosition::TopRight &&
        settings.cameraPosition != CameraOverlayPosition::BottomLeft &&
        settings.cameraPosition != CameraOverlayPosition::BottomRight) {
        throw std::invalid_argument("camera overlay position is invalid");
    }
}

void ValidateFrame(const I420Frame& frame) {
    if (frame.width <= 0 || frame.height <= 0 || frame.width > kMaxWidth || frame.height > kMaxHeight ||
        frame.y == nullptr || frame.u == nullptr || frame.v == nullptr || frame.strideY < frame.width ||
        frame.strideU < (frame.width + 1) / 2 || frame.strideV < (frame.width + 1) / 2) {
        throw std::invalid_argument("I420 frame planes or strides are invalid");
    }
}

Rect FitEven(int sourceWidth, int sourceHeight, int boxWidth, int boxHeight) {
    const int64_t widthScale = static_cast<int64_t>(boxWidth) * sourceHeight;
    const int64_t heightScale = static_cast<int64_t>(boxHeight) * sourceWidth;
    int width = boxWidth;
    int height = boxHeight;
    if (widthScale <= heightScale) {
        height = static_cast<int>(static_cast<int64_t>(sourceHeight) * boxWidth / sourceWidth);
    } else {
        width = static_cast<int>(static_cast<int64_t>(sourceWidth) * boxHeight / sourceHeight);
    }
    width = std::clamp(width & ~1, 2, boxWidth);
    height = std::clamp(height & ~1, 2, boxHeight);
    return {0, 0, width, height};
}

void ScalePlane(const uint8_t* source, int sourceStride, int sourceWidth, int sourceHeight,
                uint8_t* destination, int destinationStride, const Rect& rect) {
    for (int y = 0; y < rect.height; ++y) {
        const int sourceY = std::min(sourceHeight - 1, y * sourceHeight / rect.height);
        uint8_t* destinationRow = destination + static_cast<size_t>(rect.y + y) * destinationStride + rect.x;
        const uint8_t* sourceRow = source + static_cast<size_t>(sourceY) * sourceStride;
        for (int x = 0; x < rect.width; ++x) {
            const int sourceX = std::min(sourceWidth - 1, x * sourceWidth / rect.width);
            destinationRow[x] = sourceRow[sourceX];
        }
    }
}

void BlitFit(const I420Frame& source, ComposedVideoFrame& output, Rect box, bool fit = true) {
    Rect fitted = fit ? FitEven(source.width, source.height, box.width, box.height)
                      : Rect{0, 0, box.width, box.height};
    fitted.x = box.x + ((box.width - fitted.width) / 2 & ~1);
    fitted.y = box.y + ((box.height - fitted.height) / 2 & ~1);
    ScalePlane(source.y, source.strideY, source.width, source.height, output.y.data(), output.width, fitted);
    const Rect chromaRect{fitted.x / 2, fitted.y / 2, fitted.width / 2, fitted.height / 2};
    ScalePlane(source.u, source.strideU, (source.width + 1) / 2, (source.height + 1) / 2,
               output.u.data(), output.width / 2, chromaRect);
    ScalePlane(source.v, source.strideV, (source.width + 1) / 2, (source.height + 1) / 2,
               output.v.data(), output.width / 2, chromaRect);
}

int OverlayPercent(CameraOverlaySize size) {
    switch (size) {
    case CameraOverlaySize::Small: return 20;
    case CameraOverlaySize::Medium: return 28;
    case CameraOverlaySize::Large: return 36;
    }
    throw std::invalid_argument("camera overlay size is invalid");
}

Rect CameraBox(const VideoCompositionSettings& settings, const I420Frame& camera) {
    const int marginX = (settings.width * 2 / 100) & ~1;
    const int marginY = (settings.height * 2 / 100) & ~1;
    const int percent = OverlayPercent(settings.cameraSize);
    const auto fitted = FitEven(camera.width, camera.height,
        std::max(2, ((settings.width * percent / 100) & ~1)),
        settings.height - 2 * marginY);
    const int width = fitted.width;
    const int height = fitted.height;
    const bool right = settings.cameraPosition == CameraOverlayPosition::TopRight ||
                       settings.cameraPosition == CameraOverlayPosition::BottomRight;
    const bool bottom = settings.cameraPosition == CameraOverlayPosition::BottomLeft ||
                        settings.cameraPosition == CameraOverlayPosition::BottomRight;
    return {right ? settings.width - marginX - width : marginX,
            bottom ? settings.height - marginY - height : marginY, width, height};
}

} // namespace

I420Frame ComposedVideoFrame::View() const noexcept {
    return {y.data(), u.data(), v.data(), width, (width + 1) / 2, (width + 1) / 2, width, height, timestamp100ns};
}

ComposedVideoFrame ComposeVideoFrame(const I420Frame* desktop, const I420Frame* camera,
                                     const VideoCompositionSettings& settings, int64_t timestamp100ns) {
    ValidateSettings(settings);
    if (desktop != nullptr) ValidateFrame(*desktop);
    if (camera != nullptr) ValidateFrame(*camera);

    ComposedVideoFrame output;
    output.width = settings.width;
    output.height = settings.height;
    output.timestamp100ns = timestamp100ns;
    output.y.assign(CheckedSize(output.width, output.height), 16);
    output.u.assign(CheckedSize(output.width / 2, output.height / 2), 128);
    output.v.assign(CheckedSize(output.width / 2, output.height / 2), 128);

    const Rect canvas{0, 0, output.width, output.height};
    if (desktop != nullptr) BlitFit(*desktop, output, canvas);
    else if (camera != nullptr) BlitFit(*camera, output, canvas);
    if (desktop != nullptr && camera != nullptr) BlitFit(*camera, output, CameraBox(settings, *camera), false);
    return output;
}

} // namespace telestudio
