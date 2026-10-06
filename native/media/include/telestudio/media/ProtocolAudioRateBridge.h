#pragma once

#include "telestudio/media/OutboundAudioMixer.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <span>

struct SpeexResamplerState_;

namespace telestudio::media {

struct ProtocolAudioRateBridgeDiagnostics {
    int toMixerInputLatencySamples = 0;
    int toMixerOutputLatencySamples = 0;
    int fromMixerInputLatencySamples = 0;
    int fromMixerOutputLatencySamples = 0;
    std::uint64_t resamplerConstructions = 0;
    std::uint64_t resamplerResets = 0;
};

class ProtocolAudioRateBridge final {
public:
    explicit ProtocolAudioRateBridge(int negotiatedRateHz);
    ~ProtocolAudioRateBridge();

    ProtocolAudioRateBridge(const ProtocolAudioRateBridge&) = delete;
    ProtocolAudioRateBridge& operator=(const ProtocolAudioRateBridge&) = delete;
    ProtocolAudioRateBridge(ProtocolAudioRateBridge&&) = delete;
    ProtocolAudioRateBridge& operator=(ProtocolAudioRateBridge&&) = delete;

    // output.samples must already contain exactly 960 elements. The method
    // performs no allocation and preserves the input's timeline tags.
    [[nodiscard]] bool ToMixer(const OwnedAudioBlock& input,
                               OwnedAudioBlock& output) noexcept;

    // output must contain exactly negotiatedRateHz / 50 writable samples.
    [[nodiscard]] bool FromMixer(const OutboundAudioBlock& input,
                                 std::span<std::int16_t> output,
                                 bool& discontinuity) noexcept;

    // Call only on the serialized lifecycle path after audio callbacks stop.
    void Reset() noexcept;
    [[nodiscard]] int NegotiatedRateHz() const noexcept;
    [[nodiscard]] ProtocolAudioRateBridgeDiagnostics Diagnostics() const noexcept;

private:
    bool ResetToMixerResampler() noexcept;
    bool ResetFromMixerResampler() noexcept;
    void FailToMixer() noexcept;
    void FailFromMixer() noexcept;

    int negotiatedRateHz_ = 0;
    SpeexResamplerState_* toMixerResampler_ = nullptr;
    SpeexResamplerState_* fromMixerResampler_ = nullptr;
    std::array<std::int16_t, 960> toMixerCarry_{};
    std::array<std::int16_t, 960> fromMixerCarry_{};
    std::size_t toMixerCarryCount_ = 0;
    std::size_t fromMixerCarryCount_ = 0;
    int toMixerInputLatencySamples_ = 0;
    int toMixerOutputLatencySamples_ = 0;
    int fromMixerInputLatencySamples_ = 0;
    int fromMixerOutputLatencySamples_ = 0;
    std::uint64_t resamplerConstructions_ = 0;
    std::atomic<std::uint64_t> resamplerResets_{0};
    bool toMixerDiscontinuityPending_ = false;
    bool fromMixerDiscontinuityPending_ = false;
    // ToMixer and FromMixer have different thread owners. The input side only
    // requests this reset; the output owner applies it before its next sample.
    std::atomic<bool> fromMixerResetPending_{false};
};

} // namespace telestudio::media
