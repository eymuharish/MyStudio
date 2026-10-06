#include "telestudio/media/AdaptiveQualityController.h"

#include <algorithm>

namespace telestudio {
namespace {

constexpr int kBitrateFloorKbps = 768;

bool IsAcute(const MediaFormat& current, const NetworkSample& sample) {
    return sample.lossPercent > 1.0 || sample.jitterMs > 30.0 || sample.oneWayLatencyMs >= 200.0 ||
           sample.encoderOverloaded || sample.repeatedNacks > 0 || sample.availableKbps < current.bitrateKbps;
}

bool IsWarning(const MediaFormat& current, const NetworkSample& sample) {
    return sample.lossPercent >= 0.1 || sample.jitterMs >= 20.0 || sample.oneWayLatencyMs >= 200.0 ||
           sample.encoderOverloaded || sample.repeatedNacks > 0 || sample.availableKbps < current.bitrateKbps;
}

void StepResolutionDown(MediaFormat& format) {
    if (format.width >= 1920 && format.height >= 1080) {
        format.width = 1280;
        format.height = 720;
    } else if (format.width >= 1280 && format.height >= 720) {
        format.width = 1024;
        format.height = 576;
    }
}

} // namespace

MediaFormat AdaptiveQualityController::Observe(const MediaFormat& current,
                                               const NetworkSample& sample,
                                               std::chrono::seconds stableFor) {
    MediaFormat next = current;
    if (IsAcute(current, sample)) {
        if (current.bitrateKbps > kBitrateFloorKbps) {
            next.bitrateKbps = std::max(kBitrateFloorKbps, current.bitrateKbps * 80 / 100);
        } else {
            StepResolutionDown(next);
        }
        return next;
    }

    if (IsWarning(current, sample) || stableFor < std::chrono::seconds{30}) {
        return next;
    }

    next.bitrateKbps = std::min(sample.availableKbps, current.bitrateKbps * 120 / 100);
    return next;
}

} // namespace telestudio
