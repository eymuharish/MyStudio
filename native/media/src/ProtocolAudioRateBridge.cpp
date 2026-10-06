#include "telestudio/media/ProtocolAudioRateBridge.h"

#include <speex/speex_resampler.h>

#include <algorithm>
#include <stdexcept>

namespace telestudio::media {
namespace {
constexpr int kMixerRateHz = 48'000;
constexpr std::size_t kMixerBlockSamples = 960;
constexpr std::int64_t kBlockDuration100ns = 200'000;
constexpr int kMaximumProcessCalls = 4;

bool IsNegotiatedRate(int rate) noexcept {
    return rate == 8'000 || rate == 16'000 || rate == 32'000 || rate == 48'000;
}

bool IsValidProtocolInput(const OwnedAudioBlock& input, int negotiatedRateHz) noexcept {
    return input.sampleRateHz == negotiatedRateHz && input.channels == 1 &&
           input.duration100ns == kBlockDuration100ns &&
           input.source == AudioTapKind::Microphone &&
           input.samples.size() == static_cast<std::size_t>(negotiatedRateHz / 50);
}

bool IsValidMixerInput(const OutboundAudioBlock& input) noexcept {
    return input.sampleRateHz == kMixerRateHz && input.channels == 1 &&
           input.duration100ns == kBlockDuration100ns;
}

template <std::size_t Capacity>
bool ProcessBlock(SpeexResamplerState_* state,
                  std::span<const std::int16_t> input,
                  std::array<std::int16_t, Capacity>& carry,
                  std::size_t& carryCount,
                  std::size_t expectedOutput) noexcept {
    if (!state || carryCount != 0 || expectedOutput > carry.size()) return false;

    std::size_t inputOffset = 0;
    for (int call = 0; call < kMaximumProcessCalls && inputOffset < input.size() &&
                       carryCount < carry.size(); ++call) {
        auto inputCount = static_cast<spx_uint32_t>(input.size() - inputOffset);
        auto outputCount = static_cast<spx_uint32_t>(carry.size() - carryCount);
        const auto error = speex_resampler_process_int(
            state, 0, input.data() + inputOffset, &inputCount,
            carry.data() + carryCount, &outputCount);
        if (error != RESAMPLER_ERR_SUCCESS) return false;
        inputOffset += inputCount;
        carryCount += outputCount;
        if (inputCount == 0 && outputCount == 0) break;
    }
    return inputOffset == input.size() && carryCount == expectedOutput;
}
}

ProtocolAudioRateBridge::ProtocolAudioRateBridge(int negotiatedRateHz)
    : negotiatedRateHz_(negotiatedRateHz) {
    if (!IsNegotiatedRate(negotiatedRateHz)) {
        throw std::invalid_argument("ProtocolAudioRateBridge requires 8/16/32/48 kHz");
    }

    int error = RESAMPLER_ERR_SUCCESS;
    toMixerResampler_ = speex_resampler_init(
        1, static_cast<spx_uint32_t>(negotiatedRateHz_), kMixerRateHz,
        SPEEX_RESAMPLER_QUALITY_DEFAULT, &error);
    if (!toMixerResampler_ || error != RESAMPLER_ERR_SUCCESS) {
        if (toMixerResampler_) speex_resampler_destroy(toMixerResampler_);
        toMixerResampler_ = nullptr;
        throw std::runtime_error("failed to create protocol-to-mixer SpeexDSP resampler");
    }
    ++resamplerConstructions_;

    error = RESAMPLER_ERR_SUCCESS;
    fromMixerResampler_ = speex_resampler_init(
        1, kMixerRateHz, static_cast<spx_uint32_t>(negotiatedRateHz_),
        SPEEX_RESAMPLER_QUALITY_DEFAULT, &error);
    if (!fromMixerResampler_ || error != RESAMPLER_ERR_SUCCESS) {
        if (fromMixerResampler_) speex_resampler_destroy(fromMixerResampler_);
        speex_resampler_destroy(toMixerResampler_);
        fromMixerResampler_ = nullptr;
        toMixerResampler_ = nullptr;
        throw std::runtime_error("failed to create mixer-to-protocol SpeexDSP resampler");
    }
    ++resamplerConstructions_;

    toMixerInputLatencySamples_ = speex_resampler_get_input_latency(toMixerResampler_);
    toMixerOutputLatencySamples_ = speex_resampler_get_output_latency(toMixerResampler_);
    fromMixerInputLatencySamples_ = speex_resampler_get_input_latency(fromMixerResampler_);
    fromMixerOutputLatencySamples_ = speex_resampler_get_output_latency(fromMixerResampler_);
}

ProtocolAudioRateBridge::~ProtocolAudioRateBridge() {
    if (toMixerResampler_) speex_resampler_destroy(toMixerResampler_);
    if (fromMixerResampler_) speex_resampler_destroy(fromMixerResampler_);
}

bool ProtocolAudioRateBridge::ResetToMixerResampler() noexcept {
    toMixerCarryCount_ = 0;
    std::fill(toMixerCarry_.begin(), toMixerCarry_.end(), 0);
    resamplerResets_.fetch_add(1, std::memory_order_relaxed);
    return toMixerResampler_ &&
           speex_resampler_reset_mem(toMixerResampler_) == RESAMPLER_ERR_SUCCESS;
}

bool ProtocolAudioRateBridge::ResetFromMixerResampler() noexcept {
    fromMixerCarryCount_ = 0;
    std::fill(fromMixerCarry_.begin(), fromMixerCarry_.end(), 0);
    resamplerResets_.fetch_add(1, std::memory_order_relaxed);
    return fromMixerResampler_ &&
           speex_resampler_reset_mem(fromMixerResampler_) == RESAMPLER_ERR_SUCCESS;
}

void ProtocolAudioRateBridge::FailToMixer() noexcept {
    ResetToMixerResampler();
    toMixerDiscontinuityPending_ = true;
    fromMixerResetPending_.store(true, std::memory_order_release);
}

void ProtocolAudioRateBridge::FailFromMixer() noexcept {
    ResetFromMixerResampler();
    fromMixerDiscontinuityPending_ = true;
}

bool ProtocolAudioRateBridge::ToMixer(const OwnedAudioBlock& input,
                                      OwnedAudioBlock& output) noexcept {
    if (!IsValidProtocolInput(input, negotiatedRateHz_) ||
        output.samples.size() != kMixerBlockSamples) {
        return false;
    }

    if (negotiatedRateHz_ == kMixerRateHz) {
        std::copy(input.samples.begin(), input.samples.end(), output.samples.begin());
        output.sampleRateHz = kMixerRateHz;
        output.channels = 1;
        output.timestamp100ns = input.timestamp100ns;
        output.duration100ns = kBlockDuration100ns;
        output.source = AudioTapKind::Microphone;
        output.callGeneration = input.callGeneration;
        output.discontinuity = input.discontinuity || toMixerDiscontinuityPending_;
        toMixerDiscontinuityPending_ = false;
        if (input.discontinuity) {
            fromMixerResetPending_.store(true, std::memory_order_release);
        }
        return true;
    }

    if (input.discontinuity) {
        if (!ResetToMixerResampler()) {
            FailToMixer();
            return false;
        }
        fromMixerResetPending_.store(true, std::memory_order_release);
    }

    if (!ProcessBlock(toMixerResampler_, input.samples, toMixerCarry_,
                      toMixerCarryCount_, kMixerBlockSamples)) {
        FailToMixer();
        return false;
    }

    std::copy_n(toMixerCarry_.begin(), kMixerBlockSamples, output.samples.begin());
    toMixerCarryCount_ = 0;
    output.sampleRateHz = kMixerRateHz;
    output.channels = 1;
    output.timestamp100ns = input.timestamp100ns;
    output.duration100ns = kBlockDuration100ns;
    output.source = AudioTapKind::Microphone;
    output.callGeneration = input.callGeneration;
    output.discontinuity = input.discontinuity || toMixerDiscontinuityPending_;
    toMixerDiscontinuityPending_ = false;
    return true;
}

bool ProtocolAudioRateBridge::FromMixer(const OutboundAudioBlock& input,
                                        std::span<std::int16_t> output,
                                        bool& discontinuity) noexcept {
    const auto expectedSamples = static_cast<std::size_t>(negotiatedRateHz_ / 50);
    if (!IsValidMixerInput(input) || output.size() != expectedSamples) return false;

    const bool resetPending = fromMixerResetPending_.exchange(false, std::memory_order_acq_rel);
    if (negotiatedRateHz_ == kMixerRateHz) {
        std::copy(input.samples.begin(), input.samples.end(), output.begin());
        discontinuity = input.discontinuity || resetPending || fromMixerDiscontinuityPending_;
        fromMixerDiscontinuityPending_ = false;
        return true;
    }

    if (input.discontinuity || resetPending) {
        if (!ResetFromMixerResampler()) {
            FailFromMixer();
            return false;
        }
    }

    if (!ProcessBlock(fromMixerResampler_, input.samples, fromMixerCarry_,
                      fromMixerCarryCount_, expectedSamples)) {
        FailFromMixer();
        return false;
    }

    std::copy_n(fromMixerCarry_.begin(), expectedSamples, output.begin());
    fromMixerCarryCount_ = 0;
    discontinuity = input.discontinuity || resetPending || fromMixerDiscontinuityPending_;
    fromMixerDiscontinuityPending_ = false;
    return true;
}

void ProtocolAudioRateBridge::Reset() noexcept {
    ResetToMixerResampler();
    ResetFromMixerResampler();
    toMixerDiscontinuityPending_ = true;
    fromMixerDiscontinuityPending_ = true;
    fromMixerResetPending_.store(false, std::memory_order_release);
}

int ProtocolAudioRateBridge::NegotiatedRateHz() const noexcept {
    return negotiatedRateHz_;
}

ProtocolAudioRateBridgeDiagnostics ProtocolAudioRateBridge::Diagnostics() const noexcept {
    return {toMixerInputLatencySamples_, toMixerOutputLatencySamples_,
            fromMixerInputLatencySamples_, fromMixerOutputLatencySamples_,
            resamplerConstructions_, resamplerResets_.load(std::memory_order_relaxed)};
}

} // namespace telestudio::media
