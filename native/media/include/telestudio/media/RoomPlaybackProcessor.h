#pragma once

#include "telestudio/media/AudioTypes.h"

#include <atomic>
#include <cstdint>
#include <memory>

namespace telestudio::media {
class INoiseSuppressor;

struct RoomMixerState {
    int masterPercent = 100;
    int bassDb = 0;
    int voiceDb = 0;
    int trebleDb = 0;
    bool muted = false;
    int noiseReductionDb = 0;
    friend bool operator==(const RoomMixerState&, const RoomMixerState&) = default;
};

struct RoomMeterSnapshot {
    std::uint16_t preLimiterPeakQ15 = 0;
    std::uint16_t postLimiterPeakQ15 = 0;
    std::uint16_t gainReductionQ15 = 0;
    bool limiterActive = false;
    friend bool operator==(const RoomMeterSnapshot&, const RoomMeterSnapshot&) = default;
};

class RoomPlaybackProcessor {
public:
    RoomPlaybackProcessor() noexcept;
    ~RoomPlaybackProcessor();

    RoomMixerState ConfirmedState() const noexcept;
    bool TrySetState(RoomMixerState state) noexcept;
    bool ObserveSystemAudio(const OwnedAudioBlock& block) noexcept;
    bool Process(OwnedAudioBlock& block) noexcept;
    RoomMeterSnapshot MeterSnapshot() const noexcept;

private:
    struct Biquad {
        double Process(double input) noexcept;
        void SetCoefficients(double newB0, double newB1, double newB2,
                             double newA1, double newA2) noexcept;
        void Reset() noexcept;

        double b0 = 1.0;
        double b1 = 0.0;
        double b2 = 0.0;
        double a1 = 0.0;
        double a2 = 0.0;
        double z1 = 0.0;
        double z2 = 0.0;
    };

    static std::uint32_t PackState(RoomMixerState state) noexcept;
    static RoomMixerState UnpackState(std::uint32_t packed) noexcept;
    void Configure(RoomMixerState state, std::uint32_t packedState, int sampleRateHz) noexcept;
    void ResetSignalState() noexcept;

    std::atomic<std::uint32_t> state_;
    std::atomic<std::uint64_t> meters_{0};
    Biquad bass_;
    Biquad voice_;
    Biquad treble_;
    std::uint32_t configuredState_ = 0xffff'ffffU;
    int configuredRateHz_ = 0;
    double limiterGain_ = 1.0;
    std::unique_ptr<INoiseSuppressor> noiseSuppressor_;
    int noiseReductionDb_ = 0;
};

} // namespace telestudio::media
