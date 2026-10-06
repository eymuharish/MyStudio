#pragma once

#include "telestudio/media/AudioTypes.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace telestudio {

enum class RecordingState { Idle, Starting, Recording, Finalizing, Completed, Failed };
enum class RecordingError {
    None, AlreadyRecording, InvalidTarget, MicrophoneUnavailable, RoomAudioUnavailable,
    CaptureFailed, EncoderFailed, SinkFailed, AudioOverflow, FinalizeFailed,
    FinalizeTimeout, RenameFailed
};

struct RecordingOptions {
    std::string sourceId;
    bool includeMicrophone = true;
    bool includeRoomAudio = false;
};

struct RecordingSnapshot {
    RecordingState state = RecordingState::Idle;
    bool micIncluded = false;
    bool roomIncluded = false;
    std::int64_t elapsedMs = 0;
    std::string fileName;
    bool paused = false;
};

struct RecordingResult : RecordingSnapshot {
    RecordingError error = RecordingError::None;
};

struct MixedAudioBlock {
    std::vector<std::int16_t> samples;
    std::int64_t timestamp100ns = 0;
    std::int64_t duration100ns = 200'000;
    bool discontinuity = false;
};

enum class AudioMixStatus { Accepted, Ignored, Malformed, Stale, Overflow, TimingLost };

struct AudioMixResult {
    AudioMixStatus status = AudioMixStatus::Ignored;
    std::vector<MixedAudioBlock> blocks;
    std::vector<std::int64_t> releasedInputTimestamps100ns;
    bool warning = false;
};

struct RecordingVideoFormat {
    int width = 0;
    int height = 0;
    int fps = 0;
    int bitrateKbps = 0;
    bool highProfile = false;
    std::vector<std::uint8_t> sequenceHeader;
};

struct RecordingVideoSample {
    std::vector<std::uint8_t> annexB;
    std::int64_t timestamp100ns = 0;
    std::int64_t duration100ns = 0;
    bool cleanPoint = false;
    bool discontinuity = false;
};

struct RecordingAudioSample {
    std::vector<std::int16_t> pcm;
    std::int64_t timestamp100ns = 0;
    std::int64_t duration100ns = 200'000;
    bool discontinuity = false;
};

bool IsSafeRecordingBasename(std::string_view value, std::string_view extension = {});

} // namespace telestudio
