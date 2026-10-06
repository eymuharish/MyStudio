#include "telestudio/media/FrameConverter.h"

#include <libyuv/convert.h>
#include <libyuv/scale.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace telestudio {
namespace {

size_t CheckedMultiply(size_t left, size_t right, const char* message) {
    if (right != 0 && left > std::numeric_limits<size_t>::max() / right) {
        throw std::invalid_argument(message);
    }
    return left * right;
}

void ValidateBgra(std::span<const uint8_t> bgra, int width, int height, int stride) {
    if (width <= 0 || height <= 0) throw std::invalid_argument("BGRA dimensions must be positive");
    const size_t rowBytes = CheckedMultiply(static_cast<size_t>(width), 4, "BGRA row size overflow");
    if (stride < 0 || static_cast<size_t>(stride) < rowBytes) {
        throw std::invalid_argument("BGRA stride is smaller than one pixel row");
    }
    const size_t previousRows = CheckedMultiply(static_cast<size_t>(height - 1),
                                                static_cast<size_t>(stride),
                                                "BGRA buffer size overflow");
    if (previousRows > std::numeric_limits<size_t>::max() - rowBytes ||
        bgra.size() < previousRows + rowBytes) {
        throw std::invalid_argument("BGRA buffer is truncated");
    }
}

} // namespace

std::span<const uint8_t> I420Buffer::Y() const noexcept { return y_; }
std::span<const uint8_t> I420Buffer::U() const noexcept { return u_; }
std::span<const uint8_t> I420Buffer::V() const noexcept { return v_; }
size_t I420Buffer::YSize() const noexcept { return y_.size(); }
size_t I420Buffer::USize() const noexcept { return u_.size(); }
size_t I420Buffer::VSize() const noexcept { return v_.size(); }
int I420Buffer::Width() const noexcept { return width_; }
int I420Buffer::Height() const noexcept { return height_; }
int64_t I420Buffer::Timestamp100ns() const noexcept { return timestamp100ns_; }

I420Frame I420Buffer::View() const noexcept {
    return {y_.data(), u_.data(), v_.data(), strideY_, strideU_, strideV_,
            width_, height_, timestamp100ns_};
}

I420Buffer ConvertBgraToI420(std::span<const uint8_t> bgra,
                             int width,
                             int height,
                             int bgraStride,
                             int64_t timestamp100ns) {
    ValidateBgra(bgra, width, height, bgraStride);
    if ((width & 1) != 0 || (height & 1) != 0) {
        throw std::invalid_argument("BGRA dimensions must be even");
    }

    I420Buffer result;
    result.width_ = width;
    result.height_ = height;
    result.strideY_ = width;
    result.strideU_ = (width + 1) / 2;
    result.strideV_ = result.strideU_;
    result.timestamp100ns_ = timestamp100ns;
    result.y_.resize(CheckedMultiply(static_cast<size_t>(width), static_cast<size_t>(height),
                                     "I420 luma size overflow"));
    const size_t chromaSize = CheckedMultiply(static_cast<size_t>((width + 1) / 2),
                                              static_cast<size_t>((height + 1) / 2),
                                              "I420 chroma size overflow");
    result.u_.resize(chromaSize);
    result.v_.resize(chromaSize);

    const int conversionResult = libyuv::ARGBToI420(
        bgra.data(), bgraStride,
        result.y_.data(), result.strideY_,
        result.u_.data(), result.strideU_,
        result.v_.data(), result.strideV_,
        width, height);
    if (conversionResult != 0) {
        throw std::runtime_error("libyuv ARGBToI420 conversion failed");
    }
    return result;
}

I420Buffer ConvertBgraToRecordingI420(std::span<const uint8_t> bgra,
                                      int sourceWidth,
                                      int sourceHeight,
                                      int bgraStride,
                                      int canvasWidth,
                                      int canvasHeight,
                                      int64_t timestamp100ns) {
    ValidateBgra(bgra, sourceWidth, sourceHeight, bgraStride);
    if (canvasWidth <= 0 || canvasHeight <= 0 || canvasWidth > 1920 || canvasHeight > 1080 ||
        (canvasWidth & 1) != 0 || (canvasHeight & 1) != 0) {
        throw std::invalid_argument("Recording canvas must be positive, even, and at most 1920x1080");
    }

    I420Buffer source;
    source.width_ = sourceWidth;
    source.height_ = sourceHeight;
    source.strideY_ = sourceWidth;
    source.strideU_ = (sourceWidth + 1) / 2;
    source.strideV_ = source.strideU_;
    source.timestamp100ns_ = timestamp100ns;
    source.y_.resize(CheckedMultiply(static_cast<size_t>(sourceWidth), static_cast<size_t>(sourceHeight),
                                     "I420 luma size overflow"));
    const size_t sourceChroma = CheckedMultiply(static_cast<size_t>((sourceWidth + 1) / 2),
                                                static_cast<size_t>((sourceHeight + 1) / 2),
                                                "I420 chroma size overflow");
    source.u_.resize(sourceChroma);
    source.v_.resize(sourceChroma);
    if (libyuv::ARGBToI420(bgra.data(), bgraStride,
                           source.y_.data(), source.strideY_, source.u_.data(), source.strideU_,
                           source.v_.data(), source.strideV_, sourceWidth, sourceHeight) != 0) {
        throw std::runtime_error("libyuv ARGBToI420 conversion failed");
    }

    const std::int64_t byWidth = static_cast<std::int64_t>(canvasWidth) * sourceHeight;
    const std::int64_t byHeight = static_cast<std::int64_t>(canvasHeight) * sourceWidth;
    int fittedWidth = canvasWidth;
    int fittedHeight = canvasHeight;
    if (byWidth <= byHeight) {
        fittedHeight = static_cast<int>((static_cast<std::int64_t>(sourceHeight) * canvasWidth) / sourceWidth);
    } else {
        fittedWidth = static_cast<int>((static_cast<std::int64_t>(sourceWidth) * canvasHeight) / sourceHeight);
    }
    fittedWidth = std::clamp(fittedWidth & ~1, 2, canvasWidth);
    fittedHeight = std::clamp(fittedHeight & ~1, 2, canvasHeight);
    const int offsetX = ((canvasWidth - fittedWidth) / 2) & ~1;
    const int offsetY = ((canvasHeight - fittedHeight) / 2) & ~1;

    I420Buffer output;
    output.width_ = canvasWidth;
    output.height_ = canvasHeight;
    output.strideY_ = canvasWidth;
    output.strideU_ = canvasWidth / 2;
    output.strideV_ = canvasWidth / 2;
    output.timestamp100ns_ = timestamp100ns;
    output.y_.assign(CheckedMultiply(static_cast<size_t>(canvasWidth), static_cast<size_t>(canvasHeight),
                                     "I420 luma size overflow"), 16);
    const size_t outputChroma = CheckedMultiply(static_cast<size_t>(canvasWidth / 2),
                                                static_cast<size_t>(canvasHeight / 2),
                                                "I420 chroma size overflow");
    output.u_.assign(outputChroma, 128);
    output.v_.assign(outputChroma, 128);

    if (libyuv::I420Scale(source.y_.data(), source.strideY_, source.u_.data(), source.strideU_,
                          source.v_.data(), source.strideV_, sourceWidth, sourceHeight,
                          output.y_.data() + offsetY * output.strideY_ + offsetX, output.strideY_,
                          output.u_.data() + (offsetY / 2) * output.strideU_ + offsetX / 2,
                          output.strideU_,
                          output.v_.data() + (offsetY / 2) * output.strideV_ + offsetX / 2,
                          output.strideV_, fittedWidth, fittedHeight, libyuv::kFilterBox) != 0) {
        throw std::runtime_error("libyuv I420Scale failed");
    }
    return output;
}

} // namespace telestudio
