#include "telestudio/media/RoomPlaybackProcessor.h"

#include <atomic>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using telestudio::media::RoomMixerState;
using telestudio::media::RoomMeterSnapshot;
using telestudio::media::RoomPlaybackProcessor;

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

template<class State>
bool SetNoiseReduction(State& state, int db) {
    if constexpr (requires { state.noiseReductionDb; }) {
        state.noiseReductionDb = db;
        return true;
    }
    return false;
}

void VerifyNoiseReduction() {
    RoomPlaybackProcessor processor;
    RoomMixerState state;
    Require(SetNoiseReduction(state, 30), "room mixer has no adjustable noise reduction");
    Require(processor.TrySetState(state) && processor.ConfirmedState() == state,
            "30 dB room noise reduction was not confirmed");
    for (int invalid : {-1, 31}) {
        auto bad = state;
        SetNoiseReduction(bad, invalid);
        Require(!processor.TrySetState(bad) && processor.ConfirmedState() == state,
                "invalid room noise reduction altered state");
    }
    std::uint32_t seed = 1234567;
    double inputEnergy = 0, outputEnergy = 0;
    for (int frame = 0; frame < 300; ++frame) {
        telestudio::media::OwnedAudioBlock block;
        block.sampleRateHz = 16000; block.channels = 1; block.duration100ns = 200000;
        block.source = telestudio::media::AudioTapKind::Room;
        block.samples.resize(320);
        for (auto& sample : block.samples) {
            seed = seed * 1664525U + 1013904223U;
            sample = static_cast<std::int16_t>(static_cast<int>((seed >> 16) & 4095) - 2048);
            if (frame > 100) inputEnergy += static_cast<double>(sample) * sample;
        }
        const auto count = block.samples.size();
        Require(processor.Process(block) && block.samples.size() == count,
                "noise reduction dropped or resized a room block");
        if (frame > 100) for (auto sample : block.samples) outputEnergy += static_cast<double>(sample) * sample;
    }
    Require(outputEnergy < inputEnergy * 0.7, "room noise reduction did not reduce seeded noise energy");
    SetNoiseReduction(state, 0);
    Require(processor.TrySetState(state), "room noise bypass rejected");
    telestudio::media::OwnedAudioBlock bypass{std::vector<std::int16_t>(320, 1234), 16000, 1, 0, 200000,
        telestudio::media::AudioTapKind::Room, 0, false};
    const auto original = bypass.samples;
    Require(processor.Process(bypass) && bypass.samples == original, "noise reduction zero is not bit-identical bypass");
}

telestudio::media::OwnedAudioBlock Block(
    telestudio::media::AudioTapKind source = telestudio::media::AudioTapKind::Room,
    std::int16_t value = 1'000,
    int rate = 48'000) {
    return {
        std::vector<std::int16_t>(static_cast<std::size_t>(rate / 50), value),
        rate,
        1,
        1'234'567,
        200'000,
        source,
        42,
        false,
    };
}

telestudio::media::OwnedAudioBlock SineBlock(double frequencyHz, double amplitude = 1'000.0) {
    auto block = Block(telestudio::media::AudioTapKind::Room, 0, 48'000);
    constexpr double pi = 3.14159265358979323846;
    for (std::size_t i = 0; i < block.samples.size(); ++i) {
        block.samples[i] = static_cast<std::int16_t>(
            std::lround(amplitude * std::sin(2.0 * pi * frequencyHz * static_cast<double>(i) / 48'000.0)));
    }
    return block;
}

double Rms(const std::vector<std::int16_t>& samples) {
    double sumSquares = 0.0;
    for (const auto sample : samples) {
        const auto value = static_cast<double>(sample);
        sumSquares += value * value;
    }
    return std::sqrt(sumSquares / static_cast<double>(samples.size()));
}

double SettledRms(RoomMixerState state, double frequencyHz) {
    RoomPlaybackProcessor processor;
    Require(processor.TrySetState(state), "EQ test state was rejected");
    double result = 0.0;
    for (int i = 0; i < 15; ++i) {
        auto block = SineBlock(frequencyHz);
        Require(processor.Process(block), "EQ sine block was rejected");
        if (i == 14) result = Rms(block.samples);
    }
    return result;
}

void VerifyDefaultsAndAtomicSnapshots() {
    Require(std::atomic<std::uint32_t>::is_always_lock_free &&
                std::atomic<std::uint64_t>::is_always_lock_free,
            "room control or meter atomics are not lock-free on this target");
    RoomPlaybackProcessor processor;
    Require(processor.ConfirmedState() == RoomMixerState{100, 0, 0, 0, false},
            "default room mixer state is wrong");

    std::atomic<bool> stop{false};
    std::thread writer([&] {
        for (int i = 0; i < 20'000; ++i) {
            const auto edge = (i & 1) == 0 ? -12 : 12;
            Require(processor.TrySetState({i % 301, edge, -edge, edge, (i & 2) != 0}),
                    "valid concurrent room mixer state was rejected");
        }
        stop.store(true, std::memory_order_release);
    });

    while (!stop.load(std::memory_order_acquire)) {
        const auto state = processor.ConfirmedState();
        Require(state.masterPercent >= 0 && state.masterPercent <= 300 &&
                    state.bassDb >= -12 && state.bassDb <= 12 &&
                    state.voiceDb >= -12 && state.voiceDb <= 12 &&
                    state.trebleDb >= -12 && state.trebleDb <= 12,
                "atomic room mixer snapshot was torn");
    }
    writer.join();
}

void VerifyValidationIsTransactional() {
    RoomPlaybackProcessor processor;
    for (const auto state : {
             RoomMixerState{0, -12, -12, -12, false},
             RoomMixerState{100, 12, 12, 12, true},
             RoomMixerState{300, -12, 12, -12, false},
         }) {
        Require(processor.TrySetState(state), "valid room mixer endpoint was rejected");
        Require(processor.ConfirmedState() == state, "valid room mixer state was not confirmed");
    }

    const auto lastValid = processor.ConfirmedState();
    for (const auto invalid : {
             RoomMixerState{-1, 0, 0, 0, false},
             RoomMixerState{301, 0, 0, 0, false},
             RoomMixerState{100, -13, 0, 0, false},
             RoomMixerState{100, 13, 0, 0, false},
             RoomMixerState{100, 0, -13, 0, false},
             RoomMixerState{100, 0, 13, 0, false},
             RoomMixerState{100, 0, 0, -13, false},
             RoomMixerState{100, 0, 0, 13, false},
         }) {
        Require(!processor.TrySetState(invalid), "invalid room mixer state was accepted");
        Require(processor.ConfirmedState() == lastValid,
                "rejected room mixer state changed the confirmed state");
    }
}

void VerifyInvalidInputIsUnchanged() {
    RoomPlaybackProcessor processor;
    auto seed = Block(telestudio::media::AudioTapKind::Room, 10'000);
    Require(processor.Process(seed), "meter seed block was rejected");
    const RoomMeterSnapshot originalMeters = processor.MeterSnapshot();
    Require(originalMeters == RoomMeterSnapshot{10'000, 10'000, 0, false},
            "meter seed did not create the expected nonzero snapshot");
    for (int malformed = 0; malformed < 5; ++malformed) {
        auto block = Block();
        if (malformed == 0) block.source = telestudio::media::AudioTapKind::Microphone;
        if (malformed == 1) block.channels = 2;
        if (malformed == 2) {
            block.sampleRateHz = 44'100;
            block.samples.resize(882);
        }
        if (malformed == 3) block.samples.pop_back();
        if (malformed == 4) block.duration100ns = 199'999;
        const auto originalSamples = block.samples;
        Require(!processor.Process(block), "malformed room block was accepted");
        Require(block.samples == originalSamples, "malformed room block samples were modified");
        Require(processor.MeterSnapshot() == originalMeters,
                "malformed room block changed the last meter snapshot");
    }
}

void VerifyNeutralAndZeroPercent() {
    RoomPlaybackProcessor processor;
    auto neutral = Block(telestudio::media::AudioTapKind::Room, 0);
    neutral.samples[0] = 1'000;
    neutral.samples[1] = -1'000;
    const auto original = neutral.samples;
    Require(processor.Process(neutral), "valid neutral room block was rejected");
    Require(neutral.samples == original, "neutral room mixer changed samples below the limiter ceiling");
    Require(processor.MeterSnapshot() == RoomMeterSnapshot{1'000, 1'000, 0, false},
            "neutral room meters are wrong");

    Require(processor.TrySetState({0, 0, 0, 0, false}), "zero-percent state was rejected");
    auto silent = Block(telestudio::media::AudioTapKind::Room, 2'000);
    Require(processor.Process(silent), "zero-percent room block was rejected");
    Require(std::all_of(silent.samples.begin(), silent.samples.end(), [](auto sample) { return sample == 0; }),
            "zero-percent room mixer did not produce silence");
    Require(processor.MeterSnapshot() == RoomMeterSnapshot{},
            "zero-percent room mixer did not clear meters");
}

void VerifyMuteTakesPrecedence() {
    RoomPlaybackProcessor processor;
    Require(processor.TrySetState({300, 12, 12, 12, true}), "muted boosted state was rejected");
    auto block = Block(telestudio::media::AudioTapKind::Room, 32'767);
    Require(processor.Process(block), "muted room block was rejected");
    Require(std::all_of(block.samples.begin(), block.samples.end(), [](auto sample) { return sample == 0; }),
            "muted room block was not silent");
    Require(processor.MeterSnapshot() == RoomMeterSnapshot{},
            "muted room block did not publish zero meters");
}

void VerifyGainAndMandatoryLimiter() {
    RoomPlaybackProcessor processor;
    Require(processor.TrySetState({300, 0, 0, 0, false}), "300-percent state was rejected");
    auto block = Block(telestudio::media::AudioTapKind::Room, 32'767);
    Require(processor.Process(block), "boosted room block was rejected");
    Require(block.samples.front() == 29'204, "first limited sample wrapped or missed the -1 dBFS ceiling");
    Require(std::all_of(block.samples.begin(), block.samples.end(), [](auto sample) {
                return sample >= -29'204 && sample <= 29'204;
            }),
            "limited room block exceeded the -1 dBFS ceiling");
    const auto meters = processor.MeterSnapshot();
    Require(meters.preLimiterPeakQ15 > meters.postLimiterPeakQ15,
            "limiter meters did not report a reduced peak");
    Require(meters.postLimiterPeakQ15 == 29'204, "post-limiter meter did not map to 29204 PCM");
    Require(meters.limiterActive && meters.gainReductionQ15 > 0,
            "limiter activation or gain reduction was not reported");

    RoomPlaybackProcessor minimumVector;
    Require(minimumVector.TrySetState({300, 0, 6, 0, false}),
            "required high-amplification vector was rejected");
    auto boostedVoice = Block(telestudio::media::AudioTapKind::Room, 32'767);
    Require(minimumVector.Process(boostedVoice), "required boosted voice block was rejected");
    Require(std::all_of(boostedVoice.samples.begin(), boostedVoice.samples.end(), [](auto sample) {
                return sample >= -29'204 && sample <= 29'204;
            }),
            "required boosted voice vector exceeded the limiter ceiling");
    Require(minimumVector.TrySetState({300, 0, 6, 0, true}),
            "required muted high-amplification vector was rejected");
    auto muted = Block(telestudio::media::AudioTapKind::Room, 32'767);
    Require(minimumVector.Process(muted) &&
                std::all_of(muted.samples.begin(), muted.samples.end(), [](auto sample) { return sample == 0; }),
            "muting only the required high-amplification vector did not silence playback");
}

void VerifyLimiterRelease() {
    RoomPlaybackProcessor processor;
    Require(processor.TrySetState({300, 0, 0, 0, false}), "release state was rejected");
    auto loud = Block(telestudio::media::AudioTapKind::Room, 32'767);
    Require(processor.Process(loud), "release priming block was rejected");

    std::int16_t firstQuiet = 0;
    std::int16_t previous = 0;
    for (int i = 0; i < 15; ++i) {
        auto quiet = Block(telestudio::media::AudioTapKind::Room, 1'000);
        Require(processor.Process(quiet), "quiet release block was rejected");
        const auto representative = quiet.samples.back();
        if (i == 0) firstQuiet = representative;
        Require(representative >= previous, "limiter release did not recover monotonically");
        Require(representative <= 3'000, "limiter release exceeded unity gain");
        previous = representative;
    }
    Require(firstQuiet <= 3'000 && previous > firstQuiet && previous >= 2'800,
            "limiter did not recover toward the neutral 300-percent output");
}

void VerifyThreeBandEqGain() {
    struct BandCase {
        double frequencyHz;
        RoomMixerState boosted;
        RoomMixerState cut;
        const char* boostMessage;
        const char* cutMessage;
    };
    for (const auto& band : {
             BandCase{100.0, {100, 12, 0, 0, false}, {100, -12, 0, 0, false},
                      "bass boost ratio is outside +12 dB tolerance", "bass cut ratio is outside -12 dB tolerance"},
             BandCase{1'500.0, {100, 0, 12, 0, false}, {100, 0, -12, 0, false},
                      "voice boost ratio is outside +12 dB tolerance", "voice cut ratio is outside -12 dB tolerance"},
             BandCase{10'000.0, {100, 0, 0, 12, false}, {100, 0, 0, -12, false},
                      "treble boost ratio is outside +12 dB tolerance", "treble cut ratio is outside -12 dB tolerance"},
         }) {
        const auto neutral = SettledRms({100, 0, 0, 0, false}, band.frequencyHz);
        const auto boostRatio = SettledRms(band.boosted, band.frequencyHz) / neutral;
        const auto cutRatio = SettledRms(band.cut, band.frequencyHz) / neutral;
        Require(boostRatio >= 3.3 && boostRatio <= 4.5, band.boostMessage);
        Require(cutRatio >= 0.22 && cutRatio <= 0.31, band.cutMessage);
    }

    const auto neutralHigh = SettledRms({100, 0, 0, 0, false}, 10'000.0);
    const auto bassAtHigh = SettledRms({100, 12, 0, 0, false}, 10'000.0) / neutralHigh;
    const auto neutralLow = SettledRms({100, 0, 0, 0, false}, 100.0);
    const auto trebleAtLow = SettledRms({100, 0, 0, 12, false}, 100.0) / neutralLow;
    constexpr double lowerTolerance = 0.8413951416;
    constexpr double upperTolerance = 1.1885022274;
    Require(bassAtHigh >= lowerTolerance && bassAtHigh <= upperTolerance,
            "bass shelf changed 10 kHz by more than 1.5 dB");
    Require(trebleAtLow >= lowerTolerance && trebleAtLow <= upperTolerance,
            "treble shelf changed 100 Hz by more than 1.5 dB");
}

void VerifyRatesAndDiscontinuityResetHistory() {
    RoomPlaybackProcessor neutral;
    for (const int rate : {8'000, 16'000, 32'000, 48'000}) {
        auto block = Block(telestudio::media::AudioTapKind::Room, 1'000, rate);
        Require(neutral.Process(block), "approved room sample rate was rejected");
        Require(std::all_of(block.samples.begin(), block.samples.end(), [](auto sample) { return sample == 1'000; }),
                "neutral processing changed an approved-rate block");
    }

    RoomPlaybackProcessor discontinuityProcessor;
    Require(discontinuityProcessor.TrySetState({100, 12, 0, 0, false}),
            "discontinuity EQ state was rejected");
    auto impulse = Block(telestudio::media::AudioTapKind::Room, 10'000, 48'000);
    Require(discontinuityProcessor.Process(impulse), "discontinuity priming signal was rejected");
    auto discontinuousSilence = Block(telestudio::media::AudioTapKind::Room, 0, 48'000);
    discontinuousSilence.discontinuity = true;
    Require(discontinuityProcessor.Process(discontinuousSilence), "discontinuous silence was rejected");
    Require(std::all_of(discontinuousSilence.samples.begin(), discontinuousSilence.samples.end(),
                        [](auto sample) { return sample == 0; }),
            "discontinuity did not clear filter history");
    Require(discontinuityProcessor.MeterSnapshot().postLimiterPeakQ15 == 0,
            "discontinuity silence retained a stale output meter");

    RoomPlaybackProcessor rateProcessor;
    Require(rateProcessor.TrySetState({100, 12, 0, 0, false}), "rate-change EQ state was rejected");
    auto rateImpulse = Block(telestudio::media::AudioTapKind::Room, 10'000, 48'000);
    Require(rateProcessor.Process(rateImpulse), "rate-change priming signal was rejected");
    auto newRateSilence = Block(telestudio::media::AudioTapKind::Room, 0, 16'000);
    Require(rateProcessor.Process(newRateSilence), "rate-transition silence was rejected");
    Require(std::all_of(newRateSilence.samples.begin(), newRateSilence.samples.end(),
                        [](auto sample) { return sample == 0; }),
            "sample-rate transition did not clear filter history");
}

void VerifySharedAudioPreservesRoomLevel() {
    RoomPlaybackProcessor processor;
    auto system = Block(telestudio::media::AudioTapKind::SystemAudio, 12'000, 48'000);
    system.timestamp100ns = 10'000'000;
    Require(processor.ObserveSystemAudio(system),
            "valid outgoing system audio was rejected");

    auto returned = Block(telestudio::media::AudioTapKind::Room, 12'000, 16'000);
    returned.timestamp100ns = 16'000'000;
    Require(processor.Process(returned), "returned room audio block was rejected");
    Require(Rms(returned.samples) >= 11'500.0,
            "sharing system audio reduced incoming student voices");

    Require(processor.TrySetState({200, 0, 0, 0, false}),
            "room boost while sharing was rejected");
    auto boosted = Block(telestudio::media::AudioTapKind::Room, 12'000, 16'000);
    boosted.timestamp100ns = 17'000'000;
    Require(processor.Process(boosted) && Rms(boosted.samples) >= 23'000.0,
            "shared audio prevented independent room amplification");
    Require(processor.TrySetState({100, 0, 0, 0, false}),
            "room level reset was rejected");

    auto expired = Block(telestudio::media::AudioTapKind::Room, 12'000, 16'000);
    expired.timestamp100ns = 40'000'000;
    Require(processor.Process(expired), "post-guard room audio block was rejected");
    Require(Rms(expired.samples) >= 11'500.0,
            "room audio level changed after system audio activity");

    auto wrongSource = Block(telestudio::media::AudioTapKind::Microphone, 12'000, 48'000);
    Require(!processor.ObserveSystemAudio(wrongSource),
            "microphone audio was accepted as system audio");
}
} // namespace

int main() {
    try {
        VerifyNoiseReduction();
        VerifyDefaultsAndAtomicSnapshots();
        VerifyValidationIsTransactional();
        VerifyInvalidInputIsUnchanged();
        VerifyNeutralAndZeroPercent();
        VerifyMuteTakesPrecedence();
        VerifyGainAndMandatoryLimiter();
        VerifyLimiterRelease();
        VerifyThreeBandEqGain();
        VerifyRatesAndDiscontinuityResetHistory();
        VerifySharedAudioPreservesRoomLevel();
        std::cout << "room_playback_processor_tests: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "room_playback_processor_tests: " << error.what() << '\n';
        return 1;
    }
}
