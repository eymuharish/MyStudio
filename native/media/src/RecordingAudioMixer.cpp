#include "telestudio/media/RecordingAudioMixer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <limits>
#include <speex/speex_resampler.h>
#include <stdexcept>

namespace telestudio {
namespace {
constexpr std::int32_t kLimit = 29'204;
constexpr std::size_t kMaxPendingBlocksPerSource = 25;

std::int16_t Limited(std::int32_t value) noexcept {
    return static_cast<std::int16_t>(std::clamp(value, -kLimit, kLimit));
}
}

struct RecordingAudioMixer::Impl {
    struct Resampler {
        SpeexResamplerState* state = nullptr;
        int rate = 0;
        std::deque<std::int16_t> carry;
        std::int64_t lastInputTimestamp100ns = -1;
        std::int64_t normalizedTimestamp100ns = -1;
        ~Resampler() { if (state) speex_resampler_destroy(state); }
        void Reset(int newRate) {
            if (state) speex_resampler_destroy(state);
            state = nullptr;
            rate = newRate;
            carry.clear();
            lastInputTimestamp100ns = -1;
            normalizedTimestamp100ns = -1;
            int error = RESAMPLER_ERR_SUCCESS;
            state = speex_resampler_init(1, static_cast<spx_uint32_t>(rate), 48000, 5, &error);
            if (!state || error != RESAMPLER_ERR_SUCCESS) throw std::runtime_error("resampler init failed");
            speex_resampler_skip_zeros(state);
        }
    } microphone, room;
};

RecordingAudioMixer::RecordingAudioMixer(bool mic, bool room, std::uint64_t generation,
                                         std::int64_t epoch)
    : includeMicrophone_(mic), includeRoomAudio_(room), generation_(generation), epoch_(epoch),
      impl_(std::make_unique<Impl>()) {}

RecordingAudioMixer::~RecordingAudioMixer() = default;

void RecordingAudioMixer::ResetTimeline(){
    microphone_.blocks.clear();room_.blocks.clear();
    nextExpectedTimestamp_=-1;impl_=std::make_unique<Impl>();
}

bool RecordingAudioMixer::Valid(const media::OwnedAudioBlock& block) noexcept {
    return block.channels == 1 && (block.sampleRateHz == 8000 || block.sampleRateHz == 16000 ||
           block.sampleRateHz == 32000 || block.sampleRateHz == 48000) &&
           block.duration100ns == 200'000 &&
           block.samples.size() == static_cast<std::size_t>(block.sampleRateHz / 50);
}

MixedAudioBlock RecordingAudioMixer::Convert(const media::OwnedAudioBlock& input) {
    MixedAudioBlock output;
    output.samples.resize(960);
    auto& resampler = input.source == media::AudioTapKind::Microphone ? impl_->microphone : impl_->room;
    if (input.sampleRateHz == 48000) {
        std::copy(input.samples.begin(), input.samples.end(), output.samples.begin());
    } else {
        if (!resampler.state || resampler.rate != input.sampleRateHz || input.discontinuity) {
            resampler.Reset(input.sampleRateHz);
        }
        std::array<std::int16_t, 2048> converted{};
        spx_uint32_t inputCount = static_cast<spx_uint32_t>(input.samples.size());
        spx_uint32_t outputCount = static_cast<spx_uint32_t>(converted.size());
        const int error = speex_resampler_process_int(resampler.state, 0, input.samples.data(), &inputCount,
                                                       converted.data(), &outputCount);
        if (error != RESAMPLER_ERR_SUCCESS || inputCount != input.samples.size()) {
            throw std::runtime_error("resampler failed");
        }
        resampler.carry.insert(resampler.carry.end(), converted.begin(), converted.begin() + outputCount);
        while (resampler.carry.size() < 960) resampler.carry.push_back(0);
        for (auto& sample : output.samples) { sample = resampler.carry.front(); resampler.carry.pop_front(); }
    }
    if (resampler.normalizedTimestamp100ns < 0) {
        const auto relative = input.timestamp100ns <= epoch_
            ? 0 : input.timestamp100ns - epoch_;
        resampler.normalizedTimestamp100ns =
            epoch_ + (relative / 200'000) * 200'000;
    } else {
        if (input.timestamp100ns <= resampler.lastInputTimestamp100ns)
            throw std::runtime_error("non-monotonic audio timestamp");
        const auto delta = input.timestamp100ns - resampler.lastInputTimestamp100ns;
        if (delta % 200'000 != 0)
            throw std::runtime_error("unaligned audio timestamp");
        resampler.normalizedTimestamp100ns += delta;
    }
    resampler.lastInputTimestamp100ns = input.timestamp100ns;
    output.timestamp100ns = resampler.normalizedTimestamp100ns;
    output.duration100ns = 200'000;
    output.discontinuity = input.discontinuity;
    return output;
}

AudioMixResult RecordingAudioMixer::Push(media::OwnedAudioBlock block) noexcept {
    try {
        const bool expected = (block.source == media::AudioTapKind::Microphone && includeMicrophone_) ||
                              (block.source == media::AudioTapKind::Room && includeRoomAudio_);
        if (!expected) return {AudioMixStatus::Ignored};
        if (block.callGeneration < generation_) return {AudioMixStatus::Stale};
        if (block.callGeneration != generation_ || !Valid(block)) return {AudioMixStatus::Malformed};
        auto& stream = block.source == media::AudioTapKind::Microphone ? microphone_ : room_;
        if (stream.blocks.size() >= kMaxPendingBlocksPerSource) return {AudioMixStatus::Overflow};
        MixedAudioBlock converted;
        try { converted = Convert(block); }
        catch (...) { return {AudioMixStatus::TimingLost}; }
        stream.blocks.push_back({std::move(converted), block.timestamp100ns});
        return Drain();
    } catch (...) {
        return {AudioMixStatus::Malformed};
    }
}

AudioMixResult RecordingAudioMixer::Drain() {
    AudioMixResult result{AudioMixStatus::Accepted};
    while (true) {
        MixedAudioBlock output;
        std::array<std::int64_t, 2> releasedTimestamps{};
        std::size_t releasedCount = 0;
        const auto release = [&](std::int64_t timestamp) {
            if (releasedCount == 0 || releasedTimestamps[0] != timestamp)
                releasedTimestamps[releasedCount++] = timestamp;
        };
        if (includeMicrophone_ && includeRoomAudio_) {
            if (!microphone_.blocks.empty() && !room_.blocks.empty()) {
                const auto micTime = microphone_.blocks.front().mixed.timestamp100ns;
                const auto roomTime = room_.blocks.front().mixed.timestamp100ns;
                if (micTime == roomTime) {
                    auto micEntry = std::move(microphone_.blocks.front()); microphone_.blocks.pop_front();
                    auto roomEntry = std::move(room_.blocks.front()); room_.blocks.pop_front();
                    release(micEntry.inputTimestamp100ns);
                    release(roomEntry.inputTimestamp100ns);
                    auto& mic = micEntry.mixed;
                    auto& room = roomEntry.mixed;
                    output.timestamp100ns = micTime;
                    output.discontinuity = mic.discontinuity || room.discontinuity;
                    output.samples.resize(960);
                    for (std::size_t index = 0; index < 960; ++index) {
                        output.samples[index] = Limited((static_cast<std::int32_t>(mic.samples[index]) +
                                                         static_cast<std::int32_t>(room.samples[index])) / 2);
                    }
                } else if (micTime < roomTime) {
                    auto entry = std::move(microphone_.blocks.front()); microphone_.blocks.pop_front();
                    release(entry.inputTimestamp100ns);
                    output = std::move(entry.mixed);
                    for (auto& sample : output.samples) sample = Limited(sample / 2);
                    output.discontinuity = true;
                    result.warning = true;
                } else {
                    auto entry = std::move(room_.blocks.front()); room_.blocks.pop_front();
                    release(entry.inputTimestamp100ns);
                    output = std::move(entry.mixed);
                    for (auto& sample : output.samples) sample = Limited(sample / 2);
                    output.discontinuity = true;
                    result.warning = true;
                }
            } else if (microphone_.unavailable && !room_.blocks.empty()) {
                auto entry = std::move(room_.blocks.front()); room_.blocks.pop_front(); result.warning = true;
                release(entry.inputTimestamp100ns);
                output = std::move(entry.mixed);
                if (microphone_.silenceBudget-- > 0) for (auto& sample : output.samples) sample = Limited(sample / 2);
            } else if (room_.unavailable && !microphone_.blocks.empty()) {
                auto entry = std::move(microphone_.blocks.front()); microphone_.blocks.pop_front(); result.warning = true;
                release(entry.inputTimestamp100ns);
                output = std::move(entry.mixed);
                if (room_.silenceBudget-- > 0) for (auto& sample : output.samples) sample = Limited(sample / 2);
            } else if (room_.blocks.empty() &&
                       microphone_.blocks.size() >= kMaxPendingBlocksPerSource) {
                auto entry = std::move(microphone_.blocks.front()); microphone_.blocks.pop_front();
                release(entry.inputTimestamp100ns);
                output = std::move(entry.mixed);
                for (auto& sample : output.samples) sample = Limited(sample / 2);
                output.discontinuity = true;
                result.warning = true;
            } else if (microphone_.blocks.empty() &&
                       room_.blocks.size() >= kMaxPendingBlocksPerSource) {
                auto entry = std::move(room_.blocks.front()); room_.blocks.pop_front();
                release(entry.inputTimestamp100ns);
                output = std::move(entry.mixed);
                for (auto& sample : output.samples) sample = Limited(sample / 2);
                output.discontinuity = true;
                result.warning = true;
            } else break;
        } else if (includeMicrophone_ && !microphone_.blocks.empty()) {
            auto entry = std::move(microphone_.blocks.front()); microphone_.blocks.pop_front();
            release(entry.inputTimestamp100ns);
            output = std::move(entry.mixed);
            for (auto& sample : output.samples) sample = Limited(sample);
        } else if (includeRoomAudio_ && !room_.blocks.empty()) {
            auto entry = std::move(room_.blocks.front()); room_.blocks.pop_front();
            release(entry.inputTimestamp100ns);
            output = std::move(entry.mixed);
            for (auto& sample : output.samples) sample = Limited(sample);
        } else break;
        std::int64_t relative = 0;
        if (nextExpectedTimestamp_ < 0) {
            relative = output.timestamp100ns <= epoch_ ? 0 : output.timestamp100ns - epoch_;
            nextExpectedTimestamp_ = relative;
        } else {
            if (output.timestamp100ns < epoch_) return {AudioMixStatus::TimingLost};
            relative = output.timestamp100ns - epoch_;
            if (relative < nextExpectedTimestamp_) return {AudioMixStatus::TimingLost};
            const auto gap = relative - nextExpectedTimestamp_;
            if (gap % 200'000 != 0 || gap / 200'000 > 25) return {AudioMixStatus::TimingLost};
            for (std::int64_t index = 0; index < gap / 200'000; ++index) {
                MixedAudioBlock silence;
                silence.samples.assign(960, 0);
                silence.timestamp100ns = nextExpectedTimestamp_;
                silence.duration100ns = 200'000;
                silence.discontinuity = index == 0;
                result.blocks.push_back(std::move(silence));
                nextExpectedTimestamp_ += 200'000;
                result.warning = true;
            }
        }
        output.timestamp100ns = relative;
        for (std::size_t index = 0; index < releasedCount; ++index)
            result.releasedInputTimestamps100ns.push_back(releasedTimestamps[index]);
        result.blocks.push_back(std::move(output));
        nextExpectedTimestamp_ = relative + 200'000;
    }
    return result;
}

AudioMixResult RecordingAudioMixer::MarkUnavailable(media::AudioTapKind source) noexcept {
    try {
        (source == media::AudioTapKind::Microphone ? microphone_ : room_).unavailable = true;
        auto result = Drain();
        result.warning = true;
        return result;
    } catch (...) {
        return {AudioMixStatus::Overflow};
    }
}

std::size_t RecordingAudioMixer::PendingBlocks() const noexcept {
    return microphone_.blocks.size() + room_.blocks.size();
}

} // namespace telestudio
