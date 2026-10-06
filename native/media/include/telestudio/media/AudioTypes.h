#pragma once

#include <cstdint>
#include <vector>

namespace telestudio::media {

enum class AudioTapKind { Microphone, Room, SystemAudio };

struct OwnedAudioBlock {
    std::vector<std::int16_t> samples;
    int sampleRateHz;
    int channels;
    std::int64_t timestamp100ns;
    std::int64_t duration100ns;
    AudioTapKind source;
    std::uint64_t callGeneration;
    bool discontinuity;
};

} // namespace telestudio::media
