#pragma once

#include "telestudio/media/MediaTypes.h"

#include <chrono>

namespace telestudio {

class AdaptiveQualityController {
public:
    [[nodiscard]] MediaFormat Observe(const MediaFormat& current,
                                      const NetworkSample& sample,
                                      std::chrono::seconds stableFor);
};

} // namespace telestudio
