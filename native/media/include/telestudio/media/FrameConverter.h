#pragma once

#include "telestudio/media/IVideoEncoder.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace telestudio {

class I420Buffer {
public:
    [[nodiscard]] std::span<const uint8_t> Y() const noexcept;
    [[nodiscard]] std::span<const uint8_t> U() const noexcept;
    [[nodiscard]] std::span<const uint8_t> V() const noexcept;
    [[nodiscard]] size_t YSize() const noexcept;
    [[nodiscard]] size_t USize() const noexcept;
    [[nodiscard]] size_t VSize() const noexcept;
    [[nodiscard]] int Width() const noexcept;
    [[nodiscard]] int Height() const noexcept;
    [[nodiscard]] int64_t Timestamp100ns() const noexcept;
    [[nodiscard]] I420Frame View() const noexcept;

private:
    friend I420Buffer ConvertBgraToI420(std::span<const uint8_t>, int, int, int, int64_t);
    friend I420Buffer ConvertBgraToRecordingI420(std::span<const uint8_t>, int, int, int,
                                                 int, int, int64_t);

    std::vector<uint8_t> y_;
    std::vector<uint8_t> u_;
    std::vector<uint8_t> v_;
    int width_ = 0;
    int height_ = 0;
    int strideY_ = 0;
    int strideU_ = 0;
    int strideV_ = 0;
    int64_t timestamp100ns_ = 0;
};

[[nodiscard]] I420Buffer ConvertBgraToI420(std::span<const uint8_t> bgra,
                                           int width,
                                           int height,
                                           int bgraStride,
                                           int64_t timestamp100ns);

[[nodiscard]] I420Buffer ConvertBgraToRecordingI420(std::span<const uint8_t> bgra,
                                                    int sourceWidth,
                                                    int sourceHeight,
                                                    int bgraStride,
                                                    int canvasWidth,
                                                    int canvasHeight,
                                                    int64_t timestamp100ns);

} // namespace telestudio
