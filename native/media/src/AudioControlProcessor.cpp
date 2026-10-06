#include "telestudio/media/AudioControlProcessor.h"

#include <speex/speex_preprocess.h>

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace telestudio::media {
namespace {
constexpr std::int64_t kBlockDuration100ns = 200'000;

bool IsApprovedRate(int rate) noexcept {
    return rate == 8'000 || rate == 16'000 || rate == 32'000 || rate == 48'000;
}

bool IsValidBlock(const OwnedAudioBlock& block, AudioTapKind expected) noexcept {
    return block.source == expected && block.channels == 1 && IsApprovedRate(block.sampleRateHz) &&
           block.duration100ns == kBlockDuration100ns &&
           block.samples.size() == static_cast<std::size_t>(block.sampleRateHz / 50);
}

void ApplyGain(std::span<std::int16_t> samples, int percent) noexcept {
    if (percent == 100) return;
    for (auto& sample : samples) {
        const auto scaled = (static_cast<std::int32_t>(sample) * percent) / 100;
        sample = static_cast<std::int16_t>(std::clamp(scaled,
            static_cast<std::int32_t>(std::numeric_limits<std::int16_t>::min()),
            static_cast<std::int32_t>(std::numeric_limits<std::int16_t>::max())));
    }
}

class SpeexNoiseSuppressor final : public INoiseSuppressor {
public:
    explicit SpeexNoiseSuppressor(int attenuationDb) : attenuationDb_(attenuationDb) {}
    ~SpeexNoiseSuppressor() override {
        if (state_) speex_preprocess_state_destroy(state_);
    }

    bool Process(std::span<std::int16_t> samples, int sampleRateHz) override {
        if (!IsApprovedRate(sampleRateHz) || samples.size() != static_cast<std::size_t>(sampleRateHz / 50)) return false;
        if (sampleRateHz != sampleRateHz_) {
            if (state_) speex_preprocess_state_destroy(state_);
            state_ = nullptr;
            sampleRateHz_ = 0;
            auto* replacement = speex_preprocess_state_init(sampleRateHz / 50, sampleRateHz);
            if (!replacement) return false;
            int one = 1;
            int zero = 0;
            int noiseSuppressDb = -attenuationDb_;
            const bool configured =
                speex_preprocess_ctl(replacement, SPEEX_PREPROCESS_SET_DENOISE, &one) == 0 &&
                speex_preprocess_ctl(replacement, SPEEX_PREPROCESS_SET_AGC, &zero) == 0 &&
                speex_preprocess_ctl(replacement, SPEEX_PREPROCESS_SET_VAD, &zero) == 0 &&
                speex_preprocess_ctl(replacement, SPEEX_PREPROCESS_SET_DEREVERB, &zero) == 0 &&
                speex_preprocess_ctl(replacement, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &noiseSuppressDb) == 0;
            if (!configured) {
                speex_preprocess_state_destroy(replacement);
                return false;
            }
            state_ = replacement;
            sampleRateHz_ = sampleRateHz;
        }
        return speex_preprocess_run(state_, reinterpret_cast<spx_int16_t*>(samples.data())) >= 0;
    }

private:
    int attenuationDb_;
    SpeexPreprocessState* state_ = nullptr;
    int sampleRateHz_ = 0;
};
}

std::unique_ptr<INoiseSuppressor> CreateSpeexNoiseSuppressor(int attenuationDb) {
    if (attenuationDb < 0 || attenuationDb > 30) return {};
    return std::make_unique<SpeexNoiseSuppressor>(attenuationDb);
}

std::uint32_t AudioControlProcessor::Pack(AudioControlState state) noexcept {
    return static_cast<std::uint32_t>(state.roomVolumePercent) |
           (static_cast<std::uint32_t>(state.microphoneGainPercent) << 8U) |
           (static_cast<std::uint32_t>(state.noiseIsolation ? 1U : 0U) << 16U);
}

AudioControlState AudioControlProcessor::Unpack(std::uint32_t packed) noexcept {
    return {static_cast<int>(packed & 0xffU), static_cast<int>((packed >> 8U) & 0xffU),
            ((packed >> 16U) & 1U) != 0};
}

AudioControlProcessor::AudioControlProcessor()
    : AudioControlProcessor(CreateSpeexNoiseSuppressor()) {}

AudioControlProcessor::AudioControlProcessor(std::unique_ptr<INoiseSuppressor> suppressor) noexcept
    : state_(Pack({100, 100, true})), suppressor_(std::move(suppressor)) {}

AudioControlState AudioControlProcessor::ConfirmedState() const noexcept {
    auto state = Unpack(state_.load(std::memory_order_acquire));
    state.roomVolumePercent = roomPlayback_.ConfirmedState().masterPercent;
    return state;
}

bool AudioControlProcessor::TrySetRoomVolume(int percent) noexcept {
    auto state = roomPlayback_.ConfirmedState();
    state.masterPercent = percent;
    return roomPlayback_.TrySetState(state);
}

bool AudioControlProcessor::TrySetRoomMixer(RoomMixerState state) noexcept {
    return roomPlayback_.TrySetState(state);
}

RoomMixerState AudioControlProcessor::ConfirmedRoomMixerState() const noexcept {
    return roomPlayback_.ConfirmedState();
}

RoomMeterSnapshot AudioControlProcessor::RoomMeters() const noexcept {
    return roomPlayback_.MeterSnapshot();
}

bool AudioControlProcessor::TrySetMicrophoneGain(int percent) noexcept {
    if (percent < 0 || percent > 150) return false;
    auto expected = state_.load(std::memory_order_acquire);
    for (;;) {
        auto state = Unpack(expected);
        state.microphoneGainPercent = percent;
        if (state_.compare_exchange_weak(expected, Pack(state), std::memory_order_acq_rel, std::memory_order_acquire)) return true;
    }
}

bool AudioControlProcessor::TrySetNoiseIsolation(bool enabled) noexcept {
    auto expected = state_.load(std::memory_order_acquire);
    for (;;) {
        auto state = Unpack(expected);
        state.noiseIsolation = enabled;
        if (state_.compare_exchange_weak(expected, Pack(state), std::memory_order_acq_rel, std::memory_order_acquire)) return true;
    }
}

bool AudioControlProcessor::ProcessMicrophone(OwnedAudioBlock& block) {
    if (!IsValidBlock(block, AudioTapKind::Microphone)) return false;
    const auto state = ConfirmedState();
    if (state.noiseIsolation && suppressor_) {
        // DSP failure must not discard a 20-ms voice block or stall the codec clock.
        std::array<std::int16_t, 960> original;
        std::copy(block.samples.begin(), block.samples.end(), original.begin());
        bool processed = false;
        try { processed = suppressor_->Process(block.samples, block.sampleRateHz); }
        catch (...) { /* Keep the original voice if optional DSP fails. */ }
        if (!processed) std::copy_n(original.begin(), block.samples.size(), block.samples.begin());
    }
    ApplyGain(block.samples, state.microphoneGainPercent);
    return true;
}

bool AudioControlProcessor::ObserveSystemAudio(
    const OwnedAudioBlock& block) noexcept {
    return roomPlayback_.ObserveSystemAudio(block);
}

bool AudioControlProcessor::ApplyRoomPlayback(OwnedAudioBlock& block) noexcept {
    return roomPlayback_.Process(block);
}

} // namespace telestudio::media
