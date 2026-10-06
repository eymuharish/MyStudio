#include "telestudio/media/FrameConverter.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <ranges>
#include <stdexcept>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void RequireRejected(std::span<const uint8_t> bgra, int width, int height, int stride,
                     const char* message) {
    bool rejected = false;
    try {
        (void)telestudio::ConvertBgraToI420(bgra, width, height, stride, 0);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    Require(rejected, message);
}

} // namespace

int main() {
    try {
        using namespace telestudio;

        const std::array<uint8_t, 16> bgra = {
            0, 0, 255, 255, 0, 255, 0, 255,
            255, 0, 0, 255, 255, 255, 255, 255,
        };
        const auto buffer = ConvertBgraToI420(bgra, 2, 2, 8, 1000);
        Require(buffer.Width() == 2 && buffer.Height() == 2, "Converted dimensions must be preserved");
        Require(buffer.YSize() == 4 && buffer.USize() == 1 && buffer.VSize() == 1,
                "2x2 I420 plane sizes must be exact");
        Require(buffer.Timestamp100ns() == 1000, "Conversion must preserve the timestamp");
        const std::array<uint8_t, 4> expectedY = {82, 144, 41, 235};
        Require(std::ranges::equal(buffer.Y(), expectedY),
                "2x2 luma bytes must match pinned libyuv output");
        Require(buffer.U().front() == 128 && buffer.V().front() == 128,
                "2x2 chroma bytes must match pinned libyuv output");
        const auto view = buffer.View();
        Require(view.strideY == 2 && view.strideU == 1 && view.strideV == 1,
                "I420 view strides must match owned planes");

        const std::array<uint8_t, 24> padded = {
            0, 0, 255, 255, 0, 255, 0, 255, 77, 77, 77, 77,
            255, 0, 0, 255, 255, 255, 255, 255, 99, 99, 99, 99,
        };
        const auto paddedBuffer = ConvertBgraToI420(padded, 2, 2, 12, 1001);
        Require(std::ranges::equal(paddedBuffer.Y(), buffer.Y()) &&
                    std::ranges::equal(paddedBuffer.U(), buffer.U()) &&
                    std::ranges::equal(paddedBuffer.V(), buffer.V()),
                "BGRA row padding must not affect conversion output");

        std::vector<uint8_t> copiedY;
        std::vector<uint8_t> copiedU;
        std::vector<uint8_t> copiedV;
        const auto copyDuringCallback = [&](I420Frame frame) {
            copiedY.assign(frame.y, frame.y + frame.strideY * frame.height);
            copiedU.assign(frame.u, frame.u + frame.strideU * ((frame.height + 1) / 2));
            copiedV.assign(frame.v, frame.v + frame.strideV * ((frame.height + 1) / 2));
        };
        copyDuringCallback(buffer.View());
        Require(copiedY == std::vector<uint8_t>({82, 144, 41, 235}) &&
                    copiedU == std::vector<uint8_t>({128}) && copiedV == std::vector<uint8_t>({128}),
                "A synchronous callback must be able to copy the borrowed I420 view");

        RequireRejected(bgra, 0, 2, 8, "Zero width must be rejected");
        RequireRejected(bgra, 2, 0, 8, "Zero height must be rejected");
        RequireRejected(bgra, 2, 2, 7, "A short BGRA stride must be rejected");
        RequireRejected(std::span<const uint8_t>(bgra).first(15), 2, 2, 8,
                        "A truncated BGRA buffer must be rejected");
        RequireRejected(bgra, 1, 2, 8, "Odd dimensions not covered by the contract must be rejected");

        const std::array<uint8_t, 36> oddBgra = {
            0,0,0,255, 0,0,0,255, 0,0,0,255,
            0,0,0,255, 0,0,0,255, 0,0,0,255,
            0,0,0,255, 0,0,0,255, 0,0,0,255,
        };
        const auto fitted = ConvertBgraToRecordingI420(oddBgra, 3, 3, 12, 8, 4, 777);
        Require(fitted.Width() == 8 && fitted.Height() == 4 && fitted.Timestamp100ns() == 777,
                "recording conversion must preserve a fixed even canvas and timestamp");
        Require(fitted.YSize() == 32 && fitted.USize() == 8 && fitted.VSize() == 8,
                "recording canvas plane sizes must be exact");
        bool rejectedCanvas = false;
        try { (void)ConvertBgraToRecordingI420(oddBgra, 3, 3, 12, 1922, 1080, 0); }
        catch (const std::invalid_argument&) { rejectedCanvas = true; }
        Require(rejectedCanvas, "recording canvas must not exceed 1920x1080");

        std::cout << "media_frame_converter_tests: PASS\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "media_frame_converter_tests: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
