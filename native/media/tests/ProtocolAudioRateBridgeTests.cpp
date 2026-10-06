#include "telestudio/media/ProtocolAudioRateBridge.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace {
std::atomic<bool> gTrackAllocations = false;
std::atomic<std::size_t> gAllocationCount = 0;
}

void* operator new(std::size_t size) {
    if (gTrackAllocations.load(std::memory_order_relaxed)) {
        gAllocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    if (void* memory = std::malloc(size)) return memory;
    throw std::bad_alloc();
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

namespace {
using telestudio::media::AudioTapKind;
using telestudio::media::OutboundAudioBlock;
using telestudio::media::OwnedAudioBlock;
using telestudio::media::ProtocolAudioRateBridge;

constexpr std::int64_t kBlockDuration100ns = 200'000;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

OwnedAudioBlock Block(int rate,
                      std::int16_t sample,
                      std::int64_t timestamp100ns,
                      std::uint64_t generation = 7,
                      bool discontinuity = false) {
    return {std::vector<std::int16_t>(static_cast<std::size_t>(rate / 50), sample),
            rate, 1, timestamp100ns, kBlockDuration100ns, AudioTapKind::Microphone,
            generation, discontinuity};
}

OutboundAudioBlock MixerBlock(std::int16_t sample,
                              std::int64_t timestamp100ns,
                              std::uint64_t generation = 7,
                              bool discontinuity = false) {
    OutboundAudioBlock block;
    block.samples.fill(sample);
    block.timestamp100ns = timestamp100ns;
    block.callGeneration = generation;
    block.discontinuity = discontinuity;
    return block;
}

OutboundAudioBlock MixerBlockFrom(const OwnedAudioBlock& block) {
    OutboundAudioBlock result;
    std::copy_n(block.samples.begin(), result.samples.size(), result.samples.begin());
    result.timestamp100ns = block.timestamp100ns;
    result.callGeneration = block.callGeneration;
    result.discontinuity = block.discontinuity;
    return result;
}

template <typename Samples>
bool AllSamples(const Samples& samples, std::int16_t expected) {
    return std::all_of(samples.begin(), samples.end(), [expected](std::int16_t sample) {
        return sample == expected;
    });
}

void VerifySupportedRatesAndExactBlockContract() {
    for (const int rate : {8'000, 16'000, 32'000, 48'000}) {
        ProtocolAudioRateBridge bridge(rate);
        const auto diagnostics = bridge.Diagnostics();
        Require(bridge.NegotiatedRateHz() == rate && diagnostics.resamplerConstructions == 2,
                "each bridge must own two fixed-rate resamplers");
        Require(diagnostics.toMixerInputLatencySamples >= 0 &&
                    diagnostics.toMixerOutputLatencySamples >= 0 &&
                    diagnostics.fromMixerInputLatencySamples >= 0 &&
                    diagnostics.fromMixerOutputLatencySamples >= 0,
                "SpeexDSP latency must be measured at construction");

        OwnedAudioBlock converted = Block(48'000, 0, 0);
        converted.samples.assign(960, 0);
        std::vector<std::int16_t> protocol(static_cast<std::size_t>(rate / 50));
        for (int index = 0; index < 24; ++index) {
            const auto timestamp = 3'000'000 + static_cast<std::int64_t>(index) * kBlockDuration100ns;
            const auto input = Block(rate, 1'000, timestamp, 91);
            Require(bridge.ToMixer(input, converted), "supported input block must reach the mixer");
            Require(converted.samples.size() == 960 && converted.sampleRateHz == 48'000 &&
                        converted.channels == 1 && converted.duration100ns == kBlockDuration100ns &&
                        converted.source == AudioTapKind::Microphone &&
                        converted.timestamp100ns == timestamp && converted.callGeneration == 91,
                    "ToMixer must emit exact 48-kHz microphone metadata and sample count");

            bool discontinuity = true;
            const auto outbound = MixerBlock(1'000, timestamp, 91);
            Require(bridge.FromMixer(outbound, protocol, discontinuity),
                    "valid mixer block must reach the protocol encoder");
            Require(protocol.size() == static_cast<std::size_t>(rate / 50) && !discontinuity,
                    "FromMixer must emit the negotiated 20-ms count and discontinuity state");
        }
    }
}

void VerifyRealtimeWarmupIsRetained() {
    ProtocolAudioRateBridge bridge(8'000);
    const auto latency = bridge.Diagnostics();
    Require(latency.toMixerOutputLatencySamples > 0,
            "the test requires a measured nonzero SpeexDSP output latency");
    auto input = Block(8'000, 1'000, 0);
    auto output = Block(48'000, 0, 0);
    Require(bridge.ToMixer(input, output), "warmup input must be accepted");
    Require(output.samples.front() == 0,
            "live resampling must retain the leading warmup instead of skipping zeros");
    Require(std::any_of(output.samples.begin(), output.samples.end(), [](std::int16_t sample) {
                return sample != 0;
            }), "warmup block must still advance into the current signal");
}

void VerifySineRoundTrip(int rate) {
    constexpr double frequencyHz = 1'000.0;
    constexpr double amplitude = 10'000.0;
    constexpr int blocks = 24;
    ProtocolAudioRateBridge bridge(rate);
    const auto latency = bridge.Diagnostics();
    auto input = Block(rate, 0, 0);
    auto mixer = Block(48'000, 0, 0);
    std::vector<std::int16_t> protocol(static_cast<std::size_t>(rate / 50));
    std::vector<std::int16_t> roundTrip;
    roundTrip.reserve(static_cast<std::size_t>(blocks * rate / 50));

    std::size_t sourceIndex = 0;
    for (int blockIndex = 0; blockIndex < blocks; ++blockIndex) {
        input.timestamp100ns = static_cast<std::int64_t>(blockIndex) * kBlockDuration100ns;
        for (auto& sample : input.samples) {
            const auto phase = 2.0 * std::numbers::pi * frequencyHz *
                               static_cast<double>(sourceIndex++) / static_cast<double>(rate);
            sample = static_cast<std::int16_t>(std::lround(amplitude * std::sin(phase)));
        }
        Require(bridge.ToMixer(input, mixer), "sine input must resample to the mixer");
        const auto outbound = MixerBlockFrom(mixer);
        bool discontinuity = false;
        Require(bridge.FromMixer(outbound, protocol, discontinuity),
                "mixer sine must resample to the protocol rate");
        roundTrip.insert(roundTrip.end(), protocol.begin(), protocol.end());
    }

    const auto toMixerLatencyAtProtocolRate = static_cast<std::size_t>(std::ceil(
        static_cast<double>(latency.toMixerOutputLatencySamples) * rate / 48'000.0));
    const auto measuredWarmup = toMixerLatencyAtProtocolRate +
                                static_cast<std::size_t>(latency.fromMixerOutputLatencySamples);
    const auto discard = std::min(roundTrip.size(),
                                  measuredWarmup + static_cast<std::size_t>(rate / 50));
    Require(roundTrip.size() > discard + static_cast<std::size_t>(rate / 10),
            "measured latency must leave enough settled sine samples");

    double squareSum = 0.0;
    std::size_t crossings = 0;
    for (std::size_t index = discard; index < roundTrip.size(); ++index) {
        const auto value = static_cast<double>(roundTrip[index]);
        squareSum += value * value;
        if (index > discard && roundTrip[index - 1] <= 0 && roundTrip[index] > 0) ++crossings;
    }
    const auto count = roundTrip.size() - discard;
    const auto rms = std::sqrt(squareSum / static_cast<double>(count));
    const auto expectedRms = amplitude / std::sqrt(2.0);
    const auto rmsRatio = rms / expectedRms;
    const auto seconds = static_cast<double>(count - 1) / static_cast<double>(rate);
    const auto measuredFrequency = static_cast<double>(crossings) / seconds;
    Require(rmsRatio >= 0.90 && rmsRatio <= 1.10,
            "round-trip sine RMS must remain within the bounded ratio after measured warmup");
    Require(measuredFrequency >= 990.0 && measuredFrequency <= 1'010.0,
            "round-trip sine must preserve its 1-kHz frequency");
}

void VerifySineRoundTripsPreserveRateDirection() {
    VerifySineRoundTrip(8'000);
    VerifySineRoundTrip(16'000);
}

void VerifyMalformedInputsLeaveStreamingStateUntouched() {
    const auto verifyMutation = [](auto mutate) {
        ProtocolAudioRateBridge subject(16'000);
        ProtocolAudioRateBridge reference(16'000);
        auto subjectOutput = Block(48'000, 0, 0);
        auto referenceOutput = Block(48'000, 0, 0);
        const auto seed = Block(16'000, 900, 0);
        Require(subject.ToMixer(seed, subjectOutput) && reference.ToMixer(seed, referenceOutput),
                "state comparison setup must succeed");

        auto malformed = Block(16'000, 1'100, kBlockDuration100ns);
        mutate(malformed);
        auto sentinel = Block(48'000, 2'222, -1, 99, true);
        const auto before = sentinel;
        Require(!subject.ToMixer(malformed, sentinel), "malformed input must be rejected");
        Require(sentinel.samples == before.samples && sentinel.sampleRateHz == before.sampleRateHz &&
                    sentinel.channels == before.channels && sentinel.timestamp100ns == before.timestamp100ns &&
                    sentinel.duration100ns == before.duration100ns && sentinel.source == before.source &&
                    sentinel.callGeneration == before.callGeneration &&
                    sentinel.discontinuity == before.discontinuity,
                "malformed input must not modify the caller's output block");

        const auto next = Block(16'000, 1'300, 2 * kBlockDuration100ns);
        Require(subject.ToMixer(next, subjectOutput) && reference.ToMixer(next, referenceOutput) &&
                    subjectOutput.samples == referenceOutput.samples,
                "malformed input must not alter persistent resampler state");
    };

    verifyMutation([](OwnedAudioBlock& block) { block.sampleRateHz = 8'000; });
    verifyMutation([](OwnedAudioBlock& block) { block.samples.pop_back(); });
    verifyMutation([](OwnedAudioBlock& block) { block.channels = 2; });
    verifyMutation([](OwnedAudioBlock& block) { block.duration100ns = 199'999; });
    verifyMutation([](OwnedAudioBlock& block) { block.source = AudioTapKind::Room; });

    for (const int invalidRate : {0, 7'999, 44'100, 96'000}) {
        bool rejected = false;
        try {
            ProtocolAudioRateBridge invalid(invalidRate);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        Require(rejected, "constructor must reject rates outside 8/16/32/48 kHz");
    }
}

void VerifyMalformedMixerInputsLeaveStreamingStateUntouched() {
    const auto verifyMutation = [](auto mutate, bool wrongOutputSize) {
        ProtocolAudioRateBridge subject(16'000);
        ProtocolAudioRateBridge reference(16'000);
        std::vector<std::int16_t> subjectOutput(320, 0);
        std::vector<std::int16_t> referenceOutput(320, 0);
        bool subjectDiscontinuity = false;
        bool referenceDiscontinuity = false;
        const auto seed = MixerBlock(900, 0);
        Require(subject.FromMixer(seed, subjectOutput, subjectDiscontinuity) &&
                    reference.FromMixer(seed, referenceOutput, referenceDiscontinuity),
                "mixer-state comparison setup must succeed");

        auto malformed = MixerBlock(1'100, kBlockDuration100ns);
        mutate(malformed);
        std::vector<std::int16_t> sentinel(wrongOutputSize ? 319 : 320, 2'222);
        bool discontinuity = true;
        Require(!subject.FromMixer(malformed, sentinel, discontinuity),
                "malformed mixer input must be rejected");
        Require(AllSamples(sentinel, 2'222) && discontinuity,
                "malformed mixer input must not modify caller outputs");

        const auto next = MixerBlock(1'300, 2 * kBlockDuration100ns);
        Require(subject.FromMixer(next, subjectOutput, subjectDiscontinuity) &&
                    reference.FromMixer(next, referenceOutput, referenceDiscontinuity) &&
                    subjectOutput == referenceOutput,
                "malformed mixer input must not alter persistent resampler state");
    };

    verifyMutation([](OutboundAudioBlock& block) { block.sampleRateHz = 16'000; }, false);
    verifyMutation([](OutboundAudioBlock& block) { block.channels = 2; }, false);
    verifyMutation([](OutboundAudioBlock& block) { block.duration100ns = 199'999; }, false);
    verifyMutation([](OutboundAudioBlock&) {}, true);
}

void VerifyNearFullScaleNeverWraps() {
    for (const int rate : {8'000, 16'000, 32'000, 48'000}) {
        ProtocolAudioRateBridge bridge(rate);
        std::vector<std::int16_t> output(static_cast<std::size_t>(rate / 50));
        bool discontinuity = false;
        for (int index = 0; index < 12; ++index) {
            const auto input = MixerBlock(32'000, static_cast<std::int64_t>(index) * kBlockDuration100ns);
            Require(bridge.FromMixer(input, output, discontinuity),
                    "near-full-scale mixer block must resample successfully");
        }
        const auto [minimum, maximum] = std::minmax_element(output.begin(), output.end());
        Require(*minimum > 30'000 && *maximum <= std::numeric_limits<std::int16_t>::max(),
                "near-full-scale positive PCM must stay positive and int16 bounded");
    }
}

void VerifyDiscontinuityClearsBothDirections() {
    ProtocolAudioRateBridge bridge(16'000);
    auto mixer = Block(48'000, 0, 0);
    std::vector<std::int16_t> protocol(320);
    bool discontinuity = false;
    for (int index = 0; index < 20; ++index) {
        const auto input = Block(16'000, 8'000,
                                 static_cast<std::int64_t>(index) * kBlockDuration100ns);
        Require(bridge.ToMixer(input, mixer), "tone must settle the input resampler");
        Require(bridge.FromMixer(MixerBlockFrom(mixer), protocol, discontinuity),
                "tone must settle the output resampler");
    }
    Require(std::any_of(protocol.begin(), protocol.end(), [](std::int16_t sample) {
                return sample != 0;
            }), "discontinuity setup must contain nonzero filter history");

    const auto resetCount = bridge.Diagnostics().resamplerResets;
    const auto silence = Block(16'000, 0, 20 * kBlockDuration100ns, 8, true);
    Require(bridge.ToMixer(silence, mixer), "discontinuous zero input must be accepted");
    Require(mixer.discontinuity && mixer.callGeneration == 8 && AllSamples(mixer.samples, 0),
            "input discontinuity must clear old FIR history and propagate its tags");
    Require(bridge.FromMixer(MixerBlockFrom(mixer), protocol, discontinuity),
            "discontinuous mixer block must reach the protocol rate");
    Require(discontinuity && AllSamples(protocol, 0),
            "output discontinuity must clear old FIR history without a tone tail");
    Require(bridge.Diagnostics().resamplerResets == resetCount + 2,
            "one discontinuity must reset both per-direction resamplers exactly once");

    bridge.Reset();
    const auto nextGeneration = Block(16'000, 0, 21 * kBlockDuration100ns, 9, true);
    Require(bridge.ToMixer(nextGeneration, mixer) && AllSamples(mixer.samples, 0),
            "new generation after Reset must not emit prior tone history");
}

void VerifySteadyStateDoesNotAllocateOrReconfigure() {
    ProtocolAudioRateBridge bridge(16'000);
    auto input = Block(16'000, 1'000, 0);
    auto mixer = Block(48'000, 0, 0);
    std::vector<std::int16_t> protocol(320);
    bool discontinuity = false;
    Require(bridge.ToMixer(input, mixer) &&
                bridge.FromMixer(MixerBlockFrom(mixer), protocol, discontinuity),
            "allocation test warmup must succeed");
    const auto before = bridge.Diagnostics();

    gAllocationCount.store(0, std::memory_order_relaxed);
    gTrackAllocations.store(true, std::memory_order_release);
    bool succeeded = true;
    for (int index = 0; index < 1'000; ++index) {
        input.timestamp100ns += kBlockDuration100ns;
        succeeded = succeeded && bridge.ToMixer(input, mixer);
        auto outbound = MixerBlockFrom(mixer);
        succeeded = succeeded && bridge.FromMixer(outbound, protocol, discontinuity);
    }
    gTrackAllocations.store(false, std::memory_order_release);
    const auto after = bridge.Diagnostics();
    Require(succeeded && gAllocationCount.load(std::memory_order_relaxed) == 0,
            "1,000 ToMixer/FromMixer calls must perform no dynamic allocation");
    Require(after.resamplerConstructions == before.resamplerConstructions &&
                after.resamplerResets == before.resamplerResets,
            "steady-state callbacks must not recreate or reset either resampler");
}
}

int main() {
    try {
        VerifySupportedRatesAndExactBlockContract();
        VerifyRealtimeWarmupIsRetained();
        VerifySineRoundTripsPreserveRateDirection();
        VerifyMalformedInputsLeaveStreamingStateUntouched();
        VerifyMalformedMixerInputsLeaveStreamingStateUntouched();
        VerifyNearFullScaleNeverWraps();
        VerifyDiscontinuityClearsBothDirections();
        VerifySteadyStateDoesNotAllocateOrReconfigure();
        std::cout << "protocol_audio_rate_bridge_tests: PASS\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "protocol_audio_rate_bridge_tests: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
