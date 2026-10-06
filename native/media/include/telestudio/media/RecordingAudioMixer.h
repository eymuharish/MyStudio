#pragma once

#include "telestudio/media/RecordingTypes.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>

namespace telestudio {

class RecordingAudioMixer {
public:
    RecordingAudioMixer(bool includeMicrophone, bool includeRoomAudio,
                        std::uint64_t callGeneration, std::int64_t startEpoch100ns);
    ~RecordingAudioMixer();

    AudioMixResult Push(media::OwnedAudioBlock block) noexcept;
    AudioMixResult MarkUnavailable(media::AudioTapKind source) noexcept;
    [[nodiscard]] std::size_t PendingBlocks() const noexcept;
    void ResetTimeline();

private:
    struct StreamBlock {
        MixedAudioBlock mixed;
        std::int64_t inputTimestamp100ns = 0;
    };
    struct StreamState {
        std::deque<StreamBlock> blocks;
        bool unavailable = false;
        int silenceBudget = 25;
    };
    bool includeMicrophone_;
    bool includeRoomAudio_;
    std::uint64_t generation_;
    std::int64_t epoch_;
    std::int64_t nextExpectedTimestamp_ = -1;
    StreamState microphone_;
    StreamState room_;
    struct Impl;
    std::unique_ptr<Impl> impl_;

    static bool Valid(const media::OwnedAudioBlock& block) noexcept;
    MixedAudioBlock Convert(const media::OwnedAudioBlock& block);
    AudioMixResult Drain();
};

} // namespace telestudio
