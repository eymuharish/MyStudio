#include "telestudio/media/OutboundAudioMixer.h"

#include <algorithm>
#include <limits>

namespace telestudio::media {
namespace {
constexpr std::int64_t kBlockDuration100ns = 200'000;

std::int16_t Saturate(std::int32_t value) noexcept {
    return static_cast<std::int16_t>(std::clamp(value,
        static_cast<std::int32_t>(std::numeric_limits<std::int16_t>::min()),
        static_cast<std::int32_t>(std::numeric_limits<std::int16_t>::max())));
}
}

bool OutboundAudioMixer::IsValid(const OwnedAudioBlock& block, AudioTapKind expected) noexcept {
    return block.source == expected && block.sampleRateHz == 48'000 && block.channels == 1 &&
           block.duration100ns == kBlockDuration100ns && block.samples.size() == 960;
}

void OutboundAudioMixer::Push(InputQueue& queue, const InputBlock& block) noexcept {
    if (queue.count == queue.blocks.size()) {
        queue.first = (queue.first + 1) % queue.blocks.size();
        --queue.count;
    }
    auto& destination = queue.blocks[(queue.first + queue.count) % queue.blocks.size()];
    destination = block;
    ++queue.count;
}

void OutboundAudioMixer::DiscardBefore(InputQueue& queue, std::int64_t slot) noexcept {
    while (queue.count != 0 && queue.blocks[queue.first].slot < slot) {
        queue.first = (queue.first + 1) % queue.blocks.size();
        --queue.count;
    }
}

bool OutboundAudioMixer::TakeAt(InputQueue& queue,
                                 std::int64_t slot,
                                 InputBlock& result) noexcept {
    DiscardBefore(queue, slot);
    if (queue.count == 0 || queue.blocks[queue.first].slot != slot) return false;
    result = queue.blocks[queue.first];
    queue.first = (queue.first + 1) % queue.blocks.size();
    --queue.count;
    return true;
}

void OutboundAudioMixer::Clear(InputQueue& queue) noexcept {
    queue.first = 0;
    queue.count = 0;
}

std::int64_t OutboundAudioMixer::Bucket(std::int64_t delta100ns) noexcept {
    if (delta100ns >= 0) return (delta100ns + kBlockDuration100ns / 2) / kBlockDuration100ns;
    return (delta100ns - kBlockDuration100ns / 2) / kBlockDuration100ns;
}

void OutboundAudioMixer::Push(SourceState& source, const OwnedAudioBlock& block) noexcept {
    if (!epochSet_) {
        epochSet_ = true;
        epochTimestamp100ns_ = block.timestamp100ns;
        nextSlot_ = 0;
    }
    if (!source.epochSet || block.discontinuity || block.timestamp100ns < source.epochTimestamp100ns) {
        Clear(source.queue);
        source.epochSet = true;
        source.epochTimestamp100ns = block.timestamp100ns;
        source.epochSlot = nextSlot_;
    }

    InputBlock normalized;
    std::copy_n(block.samples.begin(), normalized.samples.size(), normalized.samples.begin());
    normalized.slot = source.epochSlot + Bucket(block.timestamp100ns - source.epochTimestamp100ns);
    normalized.callGeneration = block.callGeneration;
    normalized.discontinuity = block.discontinuity;
    Push(source.queue, normalized);
}

void OutboundAudioMixer::PushMicrophone(const OwnedAudioBlock& block) noexcept {
    std::lock_guard lock(mutex_);
    if (!microphoneMuted_ && IsValid(block, AudioTapKind::Microphone)) Push(microphone_, block);
}

void OutboundAudioMixer::PushSystemAudio(const OwnedAudioBlock& block) noexcept {
    std::lock_guard lock(mutex_);
    if (!systemAudioMuted_ && IsValid(block, AudioTapKind::SystemAudio)) Push(systemAudio_, block);
}

void OutboundAudioMixer::SetMicrophoneMuted(bool muted) noexcept {
    std::lock_guard lock(mutex_);
    microphoneMuted_ = muted;
    if (muted) Clear(microphone_.queue);
}

void OutboundAudioMixer::SetSystemAudioMuted(bool muted) noexcept {
    std::lock_guard lock(mutex_);
    systemAudioMuted_ = muted;
    if (muted) Clear(systemAudio_.queue);
}

OutboundAudioBlock OutboundAudioMixer::TakeMixedBlock() noexcept {
    OutboundAudioBlock output;
    std::lock_guard lock(mutex_);
    if (!epochSet_) return output;
    if (prebufferRemaining_ != 0) { --prebufferRemaining_; return output; }

    output.timestamp100ns = epochTimestamp100ns_ + nextSlot_ * kBlockDuration100ns;
    InputBlock microphone;
    InputBlock systemAudio;
    const bool haveMicrophone = !microphoneMuted_ && TakeAt(microphone_.queue, nextSlot_, microphone);
    const bool haveSystemAudio = !systemAudioMuted_ && TakeAt(systemAudio_.queue, nextSlot_, systemAudio);

    if (voicePriority_ && (haveMicrophone || haveSystemAudio)) {
        std::array<std::int32_t, 960> sums{};
        std::int32_t peak = 0;
        for (std::size_t index = 0; index < sums.size(); ++index) {
            const auto voice = haveMicrophone ? static_cast<std::int32_t>(microphone.samples[index]) : 0;
            const auto shared = haveSystemAudio ? static_cast<std::int32_t>(systemAudio.samples[index]) * static_cast<int>(systemPercent_) / 100 : 0;
            sums[index] = voice + shared;
            peak = std::max(peak, sums[index] < 0 ? -sums[index] : sums[index]);
        }
        for (std::size_t index = 0; index < sums.size(); ++index) {
            // One linked gain for the whole block preserves the waveform and
            // avoids clipping each sample independently. int64 prevents overflow.
            output.samples[index] = Saturate(peak > 32767
                ? static_cast<std::int32_t>(static_cast<std::int64_t>(sums[index]) * 32767 / peak)
                : sums[index]);
        }
        output.callGeneration = std::max(haveMicrophone ? microphone.callGeneration : 0,
                                         haveSystemAudio ? systemAudio.callGeneration : 0);
        output.discontinuity = (haveMicrophone && microphone.discontinuity) ||
                               (haveSystemAudio && systemAudio.discontinuity);
    } else if (haveMicrophone && haveSystemAudio) {
        for (std::size_t index = 0; index < output.samples.size(); ++index) {
            output.samples[index] = Saturate((static_cast<std::int32_t>(microphone.samples[index]) +
                                              static_cast<std::int32_t>(systemAudio.samples[index])) / 2);
        }
        output.callGeneration = std::max(microphone.callGeneration, systemAudio.callGeneration);
        output.discontinuity = microphone.discontinuity || systemAudio.discontinuity;
    } else if (haveMicrophone) {
        output.samples = microphone.samples;
        output.callGeneration = microphone.callGeneration;
        output.discontinuity = microphone.discontinuity;
    } else if (haveSystemAudio) {
        output.samples = systemAudio.samples;
        output.callGeneration = systemAudio.callGeneration;
        output.discontinuity = systemAudio.discontinuity;
    } else {
        output.callGeneration = lastGeneration_;
    }

    if (haveMicrophone || haveSystemAudio) lastGeneration_ = output.callGeneration;
    ++nextSlot_;
    return output;
}

bool OutboundAudioMixer::SetSendBufferMs(unsigned milliseconds) noexcept {
    if(milliseconds!=20 && milliseconds!=40 && milliseconds!=60 && milliseconds!=100 && milliseconds!=150 && milliseconds!=200) return false;
    std::lock_guard lock(mutex_);
    if(epochSet_) return false;
    // The mixer produces one 20ms block per callback; round 150ms upward.
    prebufferRemaining_=(milliseconds+19)/20-1;
    return true;
}

bool OutboundAudioMixer::SetVoicePriorityMix(unsigned systemPercent) noexcept {
    if (systemPercent > 100) return false;
    std::lock_guard lock(mutex_);
    voicePriority_ = true;
    systemPercent_ = systemPercent;
    return true;
}

} // namespace telestudio::media
