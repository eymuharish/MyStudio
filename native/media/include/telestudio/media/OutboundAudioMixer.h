#pragma once

#include "telestudio/media/AudioTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace telestudio::media {

struct OutboundAudioBlock {
    std::array<std::int16_t, 960> samples{};
    int sampleRateHz = 48'000;
    int channels = 1;
    std::int64_t timestamp100ns = 0;
    std::int64_t duration100ns = 200'000;
    AudioTapKind source = AudioTapKind::Microphone;
    std::uint64_t callGeneration = 0;
    bool discontinuity = false;
};

class OutboundAudioMixer {
public:
    OutboundAudioMixer() noexcept = default;

    // Every public operation is safe to call concurrently.
    void PushMicrophone(const OwnedAudioBlock& block) noexcept;
    void PushSystemAudio(const OwnedAudioBlock& block) noexcept;
    void SetMicrophoneMuted(bool muted) noexcept;
    void SetSystemAudioMuted(bool muted) noexcept;
    bool SetSendBufferMs(unsigned milliseconds) noexcept;
    // OPAL: retain microphone level, attenuate shared system audio, and limit
    // only blocks whose combined peak exceeds PCM headroom. Legacy mode is unchanged.
    bool SetVoicePriorityMix(unsigned systemPercent = 25) noexcept;
    [[nodiscard]] OutboundAudioBlock TakeMixedBlock() noexcept;

private:
    struct InputBlock {
        std::array<std::int16_t, 960> samples{};
        std::int64_t slot = 0;
        std::uint64_t callGeneration = 0;
        bool discontinuity = false;
    };

    struct InputQueue {
        std::array<InputBlock, 12> blocks{};
        std::size_t first = 0;
        std::size_t count = 0;
    };

    struct SourceState {
        InputQueue queue;
        bool epochSet = false;
        std::int64_t epochTimestamp100ns = 0;
        std::int64_t epochSlot = 0;
    };

    static bool IsValid(const OwnedAudioBlock& block, AudioTapKind expected) noexcept;
    static void Push(InputQueue& queue, const InputBlock& block) noexcept;
    static void DiscardBefore(InputQueue& queue, std::int64_t slot) noexcept;
    static bool TakeAt(InputQueue& queue, std::int64_t slot, InputBlock& result) noexcept;
    static void Clear(InputQueue& queue) noexcept;
    static std::int64_t Bucket(std::int64_t delta100ns) noexcept;
    void Push(SourceState& source, const OwnedAudioBlock& block) noexcept;

    std::mutex mutex_;
    SourceState microphone_;
    SourceState systemAudio_;
    bool microphoneMuted_ = false;
    bool systemAudioMuted_ = false;
    bool epochSet_ = false;
    std::int64_t epochTimestamp100ns_ = 0;
    std::int64_t nextSlot_ = 0;
    std::uint64_t lastGeneration_ = 0;
    unsigned prebufferRemaining_ = 0;
    bool voicePriority_ = false;
    unsigned systemPercent_ = 25;
};

} // namespace telestudio::media
