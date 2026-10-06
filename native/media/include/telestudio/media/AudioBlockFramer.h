#pragma once

#include "telestudio/media/AudioTypes.h"
#include "telestudio/media/MonotonicClock.h"

#include <cstdint>
#include <span>
#include <vector>

namespace telestudio::media {

class AudioBlockFramer {
public:
    explicit AudioBlockFramer(IMonotonicClock& clock) noexcept;
    std::vector<OwnedAudioBlock> Push(std::span<const std::int16_t> samples,
                                      int sampleRateHz,
                                      AudioTapKind source,
                                      std::uint64_t callGeneration);
    void Reset() noexcept;

private:
    void Restart(std::int64_t observation,
                 int sampleRateHz,
                 AudioTapKind source,
                 std::uint64_t callGeneration,
                 bool discontinuity);
    void AdvanceAcceptedSamples(std::size_t count) noexcept;

    IMonotonicClock& clock_;
    bool initialized_ = false;
    bool discontinuityPending_ = false;
    int sampleRateHz_ = 0;
    AudioTapKind source_ = AudioTapKind::Microphone;
    std::uint64_t callGeneration_ = 0;
    std::int64_t lastObservation100ns_ = 0;
    std::int64_t acceptedTimeline100ns_ = 0;
    std::int64_t nextBlockTimestamp100ns_ = 0;
    std::uint64_t timelineRemainder_ = 0;
    std::vector<std::int16_t> pending_;
};

} // namespace telestudio::media
