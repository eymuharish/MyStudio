#pragma once

#include "telestudio/media/AudioTypes.h"
#include "telestudio/media/RoomPlaybackProcessor.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>

namespace telestudio::media {

struct AudioControlState {
    int roomVolumePercent;
    int microphoneGainPercent;
    bool noiseIsolation;
    friend bool operator==(const AudioControlState&, const AudioControlState&) = default;
};

class INoiseSuppressor {
public:
    virtual ~INoiseSuppressor() = default;
    virtual bool Process(std::span<std::int16_t> samples, int sampleRateHz) = 0;
};

std::unique_ptr<INoiseSuppressor> CreateSpeexNoiseSuppressor(int attenuationDb = 15);

class AudioControlProcessor {
public:
    AudioControlProcessor();
    explicit AudioControlProcessor(std::unique_ptr<INoiseSuppressor> suppressor) noexcept;

    AudioControlState ConfirmedState() const noexcept;
    bool TrySetRoomVolume(int percent) noexcept;
    bool TrySetRoomMixer(RoomMixerState state) noexcept;
    RoomMixerState ConfirmedRoomMixerState() const noexcept;
    RoomMeterSnapshot RoomMeters() const noexcept;
    bool TrySetMicrophoneGain(int percent) noexcept;
    bool TrySetNoiseIsolation(bool enabled) noexcept;
    bool ProcessMicrophone(OwnedAudioBlock& block);
    bool ObserveSystemAudio(const OwnedAudioBlock& block) noexcept;
    bool ApplyRoomPlayback(OwnedAudioBlock& block) noexcept;

private:
    static std::uint32_t Pack(AudioControlState state) noexcept;
    static AudioControlState Unpack(std::uint32_t packed) noexcept;

    std::atomic<std::uint32_t> state_;
    std::unique_ptr<INoiseSuppressor> suppressor_;
    RoomPlaybackProcessor roomPlayback_;
};

} // namespace telestudio::media
