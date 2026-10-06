#include "telestudio/media/AdaptiveQualityController.h"

#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

} // namespace

int main() {
    try {
        using namespace std::chrono_literals;
        using namespace telestudio;

        AdaptiveQualityController controller;
        const MediaFormat hd{VideoCodec::H264, 1920, 1080, 30, 4000, true};
        const NetworkSample acute{1.2, 35, 210, 2500, false, 8};
        const auto bitrateReduced = controller.Observe(hd, acute, 2s);
        Require(bitrateReduced.width == 1920 && bitrateReduced.height == 1080,
                "Congestion must reduce bitrate before resolution");
        Require(bitrateReduced.bitrateKbps == 3200, "Congestion must reduce bitrate by 20 percent");

        const MediaFormat floorHd{VideoCodec::H264, 1920, 1080, 30, 768, false};
        const auto floor720 = controller.Observe(floorHd, acute, 0s);
        Require(floor720.width == 1280 && floor720.height == 720 && floor720.bitrateKbps == 768,
                "Resolution must step down at the bitrate floor");
        const MediaFormat floor720Input{VideoCodec::H264, 1280, 720, 30, 768, false};
        const auto floor576 = controller.Observe(floor720Input, acute, 0s);
        Require(floor576.width == 1024 && floor576.height == 576, "720p must step down to 576p");
        const MediaFormat floor576Input{VideoCodec::H264, 1024, 576, 30, 768, false};
        const auto floor576Again = controller.Observe(floor576Input, acute, 0s);
        Require(floor576Again.width == 1024 && floor576Again.height == 576,
                "576p must not step below its floor");

        const MediaFormat recoveryInput{VideoCodec::H264, 1280, 720, 30, 2000, false};
        const NetworkSample clean{0.01, 4, 80, 6000, false, 0};
        const auto beforeGate = controller.Observe(recoveryInput, clean, 29s);
        Require(beforeGate.bitrateKbps == 2000 && beforeGate.width == 1280 && beforeGate.height == 720,
                "Recovery must wait for 30 stable seconds");
        const auto afterGate = controller.Observe(recoveryInput, clean, 30s);
        Require(afterGate.bitrateKbps == 2400 && afterGate.width == 1280 && afterGate.height == 720,
                "Recovery must raise bitrate by 20 percent only");
        const NetworkSample warning{0.1, 4, 80, 6000, false, 0};
        const auto warned = controller.Observe(recoveryInput, warning, 30s);
        Require(warned.bitrateKbps == 2000, "Warning telemetry must block recovery");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
