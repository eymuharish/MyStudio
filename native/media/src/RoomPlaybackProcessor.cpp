#include "telestudio/media/RoomPlaybackProcessor.h"
#include "telestudio/media/AudioControlProcessor.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace telestudio::media {
namespace {
constexpr std::uint32_t kMasterMask = 0x1ffU;
constexpr unsigned kBassShift = 9U;
constexpr unsigned kVoiceShift = 14U;
constexpr unsigned kTrebleShift = 19U;
constexpr unsigned kMuteShift = 24U;
constexpr unsigned kNoiseShift = 25U;
constexpr std::int64_t kBlockDuration100ns = 200'000;
constexpr double kLimiterCeiling = 29'204.0;
constexpr double kPi = 3.14159265358979323846;

struct Coefficients {
    double b0;
    double b1;
    double b2;
    double a1;
    double a2;
};

Coefficients Normalize(double b0, double b1, double b2,
                       double a0, double a1, double a2) noexcept {
    return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

Coefficients LowShelf(int sampleRateHz, double gainDb) noexcept {
    const double amplitude = std::pow(10.0, gainDb / 40.0);
    const double omega = 2.0 * kPi * 200.0 / sampleRateHz;
    const double cosine = std::cos(omega);
    const double alpha = std::sin(omega) * std::sqrt(2.0) / 2.0;
    const double beta = 2.0 * std::sqrt(amplitude) * alpha;
    const double ap1 = amplitude + 1.0;
    const double am1 = amplitude - 1.0;
    return Normalize(
        amplitude * (ap1 - am1 * cosine + beta),
        2.0 * amplitude * (am1 - ap1 * cosine),
        amplitude * (ap1 - am1 * cosine - beta),
        ap1 + am1 * cosine + beta,
        -2.0 * (am1 + ap1 * cosine),
        ap1 + am1 * cosine - beta);
}

Coefficients Peaking(int sampleRateHz, double gainDb) noexcept {
    const double amplitude = std::pow(10.0, gainDb / 40.0);
    const double omega = 2.0 * kPi * 1'500.0 / sampleRateHz;
    const double cosine = std::cos(omega);
    const double alpha = std::sin(omega) / 2.0;
    return Normalize(
        1.0 + alpha * amplitude,
        -2.0 * cosine,
        1.0 - alpha * amplitude,
        1.0 + alpha / amplitude,
        -2.0 * cosine,
        1.0 - alpha / amplitude);
}

Coefficients HighShelf(int sampleRateHz, double gainDb) noexcept {
    const double amplitude = std::pow(10.0, gainDb / 40.0);
    const double frequency = std::min(6'000.0, 0.375 * sampleRateHz);
    const double omega = 2.0 * kPi * frequency / sampleRateHz;
    const double cosine = std::cos(omega);
    const double alpha = std::sin(omega) * std::sqrt(2.0) / 2.0;
    const double beta = 2.0 * std::sqrt(amplitude) * alpha;
    const double ap1 = amplitude + 1.0;
    const double am1 = amplitude - 1.0;
    return Normalize(
        amplitude * (ap1 + am1 * cosine + beta),
        -2.0 * amplitude * (am1 + ap1 * cosine),
        amplitude * (ap1 + am1 * cosine - beta),
        ap1 - am1 * cosine + beta,
        2.0 * (am1 - ap1 * cosine),
        ap1 - am1 * cosine - beta);
}

bool IsValidState(const RoomMixerState& state) noexcept {
    return state.masterPercent >= 0 && state.masterPercent <= 300 &&
           state.bassDb >= -12 && state.bassDb <= 12 &&
           state.voiceDb >= -12 && state.voiceDb <= 12 &&
           state.trebleDb >= -12 && state.trebleDb <= 12 &&
           state.noiseReductionDb >= 0 && state.noiseReductionDb <= 30;
}

bool IsApprovedRate(int rate) noexcept {
    return rate == 8'000 || rate == 16'000 || rate == 32'000 || rate == 48'000;
}

bool IsValidBlock(const OwnedAudioBlock& block) noexcept {
    return block.source == AudioTapKind::Room && block.channels == 1 &&
           IsApprovedRate(block.sampleRateHz) &&
           block.duration100ns == kBlockDuration100ns &&
           block.samples.size() == static_cast<std::size_t>(block.sampleRateHz / 50);
}

bool IsValidSystemAudioBlock(const OwnedAudioBlock& block) noexcept {
    return block.source == AudioTapKind::SystemAudio && block.channels == 1 &&
           block.sampleRateHz == 48'000 && block.duration100ns == kBlockDuration100ns &&
           block.samples.size() == 960 && block.timestamp100ns >= 0;
}

std::uint64_t PackMeters(RoomMeterSnapshot meters) noexcept {
    return static_cast<std::uint64_t>(meters.preLimiterPeakQ15) |
           (static_cast<std::uint64_t>(meters.postLimiterPeakQ15) << 16U) |
           (static_cast<std::uint64_t>(meters.gainReductionQ15) << 32U) |
           (static_cast<std::uint64_t>(meters.limiterActive ? 1U : 0U) << 48U);
}

std::uint16_t MeterValue(double value) noexcept {
    return static_cast<std::uint16_t>(std::clamp(std::lround(value), 0L, 65'535L));
}
} // namespace

std::uint32_t RoomPlaybackProcessor::PackState(RoomMixerState state) noexcept {
    return static_cast<std::uint32_t>(state.masterPercent) |
           (static_cast<std::uint32_t>(state.bassDb + 12) << kBassShift) |
           (static_cast<std::uint32_t>(state.voiceDb + 12) << kVoiceShift) |
           (static_cast<std::uint32_t>(state.trebleDb + 12) << kTrebleShift) |
           (static_cast<std::uint32_t>(state.muted ? 1U : 0U) << kMuteShift) |
           (static_cast<std::uint32_t>(state.noiseReductionDb) << kNoiseShift);
}

RoomMixerState RoomPlaybackProcessor::UnpackState(std::uint32_t packed) noexcept {
    return {
        static_cast<int>(packed & kMasterMask),
        static_cast<int>((packed >> kBassShift) & 0x1fU) - 12,
        static_cast<int>((packed >> kVoiceShift) & 0x1fU) - 12,
        static_cast<int>((packed >> kTrebleShift) & 0x1fU) - 12,
        ((packed >> kMuteShift) & 1U) != 0,
        static_cast<int>((packed >> kNoiseShift) & 0x1fU),
    };
}

RoomPlaybackProcessor::RoomPlaybackProcessor() noexcept
    : state_(PackState({})) {}

RoomPlaybackProcessor::~RoomPlaybackProcessor() = default;

double RoomPlaybackProcessor::Biquad::Process(double input) noexcept {
    const double output = b0 * input + z1;
    z1 = b1 * input - a1 * output + z2;
    z2 = b2 * input - a2 * output;
    return output;
}

void RoomPlaybackProcessor::Biquad::SetCoefficients(
    double newB0, double newB1, double newB2, double newA1, double newA2) noexcept {
    b0 = newB0;
    b1 = newB1;
    b2 = newB2;
    a1 = newA1;
    a2 = newA2;
}

void RoomPlaybackProcessor::Biquad::Reset() noexcept {
    z1 = 0.0;
    z2 = 0.0;
}

void RoomPlaybackProcessor::Configure(
    RoomMixerState state, std::uint32_t packedState, int sampleRateHz) noexcept {
    const auto bass = LowShelf(sampleRateHz, state.bassDb);
    const auto voice = Peaking(sampleRateHz, state.voiceDb);
    const auto treble = HighShelf(sampleRateHz, state.trebleDb);
    bass_.SetCoefficients(bass.b0, bass.b1, bass.b2, bass.a1, bass.a2);
    voice_.SetCoefficients(voice.b0, voice.b1, voice.b2, voice.a1, voice.a2);
    treble_.SetCoefficients(treble.b0, treble.b1, treble.b2, treble.a1, treble.a2);
    configuredState_ = packedState;
    configuredRateHz_ = sampleRateHz;
}

void RoomPlaybackProcessor::ResetSignalState() noexcept {
    bass_.Reset();
    voice_.Reset();
    treble_.Reset();
    limiterGain_ = 1.0;
}

RoomMixerState RoomPlaybackProcessor::ConfirmedState() const noexcept {
    return UnpackState(state_.load(std::memory_order_acquire));
}

bool RoomPlaybackProcessor::TrySetState(RoomMixerState state) noexcept {
    if (!IsValidState(state)) return false;
    auto expected = state_.load(std::memory_order_acquire);
    const auto desired = PackState(state);
    while (!state_.compare_exchange_weak(
        expected, desired, std::memory_order_acq_rel, std::memory_order_acquire)) {}
    return true;
}

bool RoomPlaybackProcessor::ObserveSystemAudio(const OwnedAudioBlock& block) noexcept {
    // Sharing must not turn down independent incoming voices. Echo control
    // belongs in the echo canceller, not a blanket gain on all room playback.
    return IsValidSystemAudioBlock(block);
}

bool RoomPlaybackProcessor::Process(OwnedAudioBlock& block) noexcept {
    if (!IsValidBlock(block)) return false;
    const auto packedState = state_.load(std::memory_order_acquire);
    const auto state = UnpackState(packedState);
    const bool rateChanged = configuredRateHz_ != block.sampleRateHz;
    if (configuredState_ != packedState || rateChanged) {
        Configure(state, packedState, block.sampleRateHz);
    }
    if (rateChanged || block.discontinuity) ResetSignalState();
    if (state.muted || state.masterPercent == 0) {
        std::fill(block.samples.begin(), block.samples.end(), 0);
        meters_.store(0, std::memory_order_release);
        return true;
    }

    if (noiseReductionDb_ != state.noiseReductionDb || rateChanged || block.discontinuity) {
        noiseSuppressor_.reset();
        noiseReductionDb_ = state.noiseReductionDb;
        try {
            if (noiseReductionDb_ > 0) noiseSuppressor_ = CreateSpeexNoiseSuppressor(noiseReductionDb_);
        } catch (...) { /* Optional noise cleanup must never interrupt room playback. */ }
    }
    if (noiseSuppressor_ && state.noiseReductionDb > 0) {
        std::array<std::int16_t, 960> original;
        std::copy(block.samples.begin(), block.samples.end(), original.begin());
        bool processed = false;
        try { processed = noiseSuppressor_->Process(block.samples, block.sampleRateHz); }
        catch (...) { /* Fail open, without dropping, gating, or resizing a block. */ }
        if (!processed) std::copy_n(original.begin(), block.samples.size(), block.samples.begin());
    }

    double prePeak = 0.0;
    double postPeak = 0.0;
    double minimumGain = limiterGain_;
    const double masterGain = static_cast<double>(state.masterPercent) / 100.0;
    const double releaseStep = 1.0 - std::exp(-1.0 / (0.100 * block.sampleRateHz));
    for (auto& sample : block.samples) {
        double filtered = bass_.Process(static_cast<double>(sample));
        filtered = voice_.Process(filtered);
        filtered = treble_.Process(filtered);
        const double preLimiter = filtered * masterGain;
        const double magnitude = std::abs(preLimiter);
        prePeak = std::max(prePeak, magnitude);
        if (magnitude > kLimiterCeiling) {
            limiterGain_ = std::min(limiterGain_, kLimiterCeiling / magnitude);
        } else {
            limiterGain_ += (1.0 - limiterGain_) * releaseStep;
        }
        minimumGain = std::min(minimumGain, limiterGain_);
        const double limited = std::clamp(
            preLimiter * limiterGain_, -kLimiterCeiling, kLimiterCeiling);
        postPeak = std::max(postPeak, std::abs(limited));
        sample = static_cast<std::int16_t>(std::lround(limited));
    }

    const auto reduction = MeterValue((1.0 - minimumGain) * 32'767.0);
    meters_.store(PackMeters({MeterValue(prePeak), MeterValue(postPeak), reduction, reduction != 0}),
                  std::memory_order_release);
    return true;
}

RoomMeterSnapshot RoomPlaybackProcessor::MeterSnapshot() const noexcept {
    const auto packed = meters_.load(std::memory_order_acquire);
    return {
        static_cast<std::uint16_t>(packed & 0xffffU),
        static_cast<std::uint16_t>((packed >> 16U) & 0xffffU),
        static_cast<std::uint16_t>((packed >> 32U) & 0xffffU),
        ((packed >> 48U) & 1U) != 0,
    };
}

} // namespace telestudio::media
