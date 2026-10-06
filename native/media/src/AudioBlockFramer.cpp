#include "telestudio/media/AudioBlockFramer.h"

#include <algorithm>
#include <limits>

namespace telestudio::media {
namespace {
constexpr std::int64_t kBlockDuration100ns = 200'000;
constexpr std::uint64_t kUnitsPerSecond100ns = 10'000'000;

bool IsApprovedSampleRate(int sampleRateHz) noexcept {
    return sampleRateHz == 8'000 || sampleRateHz == 16'000 ||
           sampleRateHz == 32'000 || sampleRateHz == 48'000;
}

std::int64_t SaturatingAdd(std::int64_t value, std::uint64_t increment) noexcept {
    constexpr auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    const auto positive = value < 0 ? 0U : static_cast<std::uint64_t>(value);
    if (increment > maximum - positive) return std::numeric_limits<std::int64_t>::max();
    return static_cast<std::int64_t>(positive + increment);
}
}

AudioBlockFramer::AudioBlockFramer(IMonotonicClock& clock) noexcept : clock_(clock) {}

void AudioBlockFramer::Reset() noexcept {
    initialized_ = false;
    discontinuityPending_ = true;
    sampleRateHz_ = 0;
    callGeneration_ = 0;
    timelineRemainder_ = 0;
    pending_.clear();
}

void AudioBlockFramer::Restart(std::int64_t observation,
                               int sampleRateHz,
                               AudioTapKind source,
                               std::uint64_t callGeneration,
                               bool discontinuity) {
    initialized_ = true;
    discontinuityPending_ = discontinuity;
    sampleRateHz_ = sampleRateHz;
    source_ = source;
    callGeneration_ = callGeneration;
    lastObservation100ns_ = observation;
    acceptedTimeline100ns_ = observation;
    nextBlockTimestamp100ns_ = observation;
    timelineRemainder_ = 0;
    pending_.clear();
}

void AudioBlockFramer::AdvanceAcceptedSamples(std::size_t count) noexcept {
    const auto rate = static_cast<std::uint64_t>(sampleRateHz_);
    const auto wholeSeconds = static_cast<std::uint64_t>(count) / rate;
    const auto sampleRemainder = static_cast<std::uint64_t>(count) % rate;
    constexpr auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    std::uint64_t delta = maximum;
    if (wholeSeconds <= maximum / kUnitsPerSecond100ns) {
        const auto whole = wholeSeconds * kUnitsPerSecond100ns;
        const auto fractionalNumerator = sampleRemainder * kUnitsPerSecond100ns + timelineRemainder_;
        const auto fractional = fractionalNumerator / rate;
        timelineRemainder_ = fractionalNumerator % rate;
        delta = fractional <= maximum - whole ? whole + fractional : maximum;
    }
    acceptedTimeline100ns_ = SaturatingAdd(acceptedTimeline100ns_, delta);
}

std::vector<OwnedAudioBlock> AudioBlockFramer::Push(std::span<const std::int16_t> samples,
                                                    int sampleRateHz,
                                                    AudioTapKind source,
                                                    std::uint64_t callGeneration) {
    std::vector<OwnedAudioBlock> blocks;
    if (samples.empty() || !IsApprovedSampleRate(sampleRateHz)) return blocks;
    if (initialized_ && callGeneration < callGeneration_) return blocks;

    const auto observation = clock_.Now100ns();
    if (!initialized_) {
        const bool afterExplicitReset = discontinuityPending_;
        Restart(observation, sampleRateHz, source, callGeneration, afterExplicitReset);
    } else if (callGeneration > callGeneration_ || sampleRateHz != sampleRateHz_ || source != source_) {
        Restart(observation, sampleRateHz, source, callGeneration, true);
    } else {
        const bool backward = observation < lastObservation100ns_;
        const bool timelineCanAdd = acceptedTimeline100ns_ <= std::numeric_limits<std::int64_t>::max() - kBlockDuration100ns;
        const bool gap = timelineCanAdd && observation > acceptedTimeline100ns_ + kBlockDuration100ns;
        if (backward || gap) Restart(observation, sampleRateHz, source, callGeneration, true);
        else lastObservation100ns_ = observation;
    }

    pending_.insert(pending_.end(), samples.begin(), samples.end());
    AdvanceAcceptedSamples(samples.size());
    const auto blockSamples = static_cast<std::size_t>(sampleRateHz_ / 50);
    std::size_t consumed = 0;
    while (pending_.size() - consumed >= blockSamples) {
        OwnedAudioBlock block;
        block.samples.assign(pending_.begin() + static_cast<std::ptrdiff_t>(consumed),
                             pending_.begin() + static_cast<std::ptrdiff_t>(consumed + blockSamples));
        block.sampleRateHz = sampleRateHz_;
        block.channels = 1;
        block.timestamp100ns = nextBlockTimestamp100ns_;
        block.duration100ns = kBlockDuration100ns;
        block.source = source_;
        block.callGeneration = callGeneration_;
        block.discontinuity = discontinuityPending_;
        discontinuityPending_ = false;
        blocks.push_back(std::move(block));
        consumed += blockSamples;
        nextBlockTimestamp100ns_ = SaturatingAdd(nextBlockTimestamp100ns_, kBlockDuration100ns);
    }
    if (consumed != 0) pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(consumed));
    return blocks;
}

} // namespace telestudio::media
