#include "telestudio/media/AudioControlProcessor.h"
#include "telestudio/media/RoomPlaybackProcessor.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

class FakeSuppressor final : public telestudio::media::INoiseSuppressor {
public:
    bool succeed = true;
    bool throwOnFailure = false;
    int calls = 0;
    std::int16_t observedFirst = 0;
    bool Process(std::span<std::int16_t> samples, int) override {
        ++calls;
        observedFirst = samples.front();
        if (!succeed) {
            samples.front() = -30000;
            if (throwOnFailure) throw std::runtime_error("DSP failure");
            return false;
        }
        for (auto& sample : samples) sample = static_cast<std::int16_t>(sample + 100);
        return true;
    }
};

telestudio::media::OwnedAudioBlock Block(telestudio::media::AudioTapKind source,
                                         std::int16_t value = 100,
                                         int rate = 8'000) {
    return {std::vector<std::int16_t>(static_cast<std::size_t>(rate / 50), value), rate, 1,
            1'234'567, 200'000, source, 42, true};
}

void VerifyStateAndAtomicSnapshots() {
    auto fake = std::make_unique<FakeSuppressor>();
    telestudio::media::AudioControlProcessor processor(std::move(fake));
    Require(processor.ConfirmedState() == telestudio::media::AudioControlState{100, 100, true}, "default audio state is wrong");
    Require(processor.TrySetRoomVolume(0) && processor.TrySetRoomVolume(100) && processor.TrySetRoomVolume(300),
            "valid room endpoints rejected");
    Require(processor.TrySetMicrophoneGain(0) && processor.TrySetMicrophoneGain(150), "valid microphone endpoints rejected");
    Require(processor.TrySetNoiseIsolation(false), "valid isolation state rejected");
    const auto confirmed = processor.ConfirmedState();
    Require(!processor.TrySetRoomVolume(-1) && !processor.TrySetRoomVolume(301), "invalid room value accepted");
    Require(!processor.TrySetMicrophoneGain(-1) && !processor.TrySetMicrophoneGain(151), "invalid microphone value accepted");
    Require(processor.ConfirmedState() == confirmed, "invalid setter changed confirmed state");

    std::atomic<bool> stop{false};
    std::thread writer([&] {
        for (int i = 0; i < 10'000; ++i) {
            processor.TrySetRoomVolume(i % 301);
            processor.TrySetMicrophoneGain(i % 151);
            processor.TrySetNoiseIsolation((i & 1) != 0);
        }
        stop = true;
    });
    while (!stop.load()) {
        const auto state = processor.ConfirmedState();
        Require(state.roomVolumePercent >= 0 && state.roomVolumePercent <= 300 &&
                state.microphoneGainPercent >= 0 && state.microphoneGainPercent <= 150,
                "atomic confirmed snapshot was torn");
    }
    writer.join();
}

void VerifyRoomMixerFacadePreservesIndependentState() {
    auto fake = std::make_unique<FakeSuppressor>();
    telestudio::media::AudioControlProcessor processor(std::move(fake));
    const telestudio::media::RoomMixerState requested{250, 12, -12, 6, true, 30};
    Require(processor.TrySetRoomMixer(requested), "valid full room mixer state was rejected");
    Require(processor.ConfirmedRoomMixerState() == requested,
            "full room mixer state was not confirmed");
    Require(processor.ConfirmedState().roomVolumePercent == 250,
            "legacy confirmed room volume did not reflect the room mixer master");
    Require(processor.TrySetRoomVolume(300), "legacy 300-percent room setter was rejected");
    Require(processor.ConfirmedRoomMixerState() == telestudio::media::RoomMixerState{300, 12, -12, 6, true, 30},
            "legacy room setter did not preserve EQ and mute state");
    Require(processor.RoomMeters() == telestudio::media::RoomMeterSnapshot{},
            "room facade did not expose the initial atomic meter snapshot");
}

void VerifyMicrophoneOrderingAndGain() {
    auto fakeOwned = std::make_unique<FakeSuppressor>();
    auto* fake = fakeOwned.get();
    telestudio::media::AudioControlProcessor processor(std::move(fakeOwned));
    Require(processor.TrySetMicrophoneGain(150), "gain setup failed");
    auto block = Block(telestudio::media::AudioTapKind::Microphone, 100);
    const auto metadata = block;
    Require(processor.ProcessMicrophone(block), "processed microphone block rejected");
    Require(fake->calls == 1 && fake->observedFirst == 100 && block.samples.front() == 300,
            "denoise must run before 150-percent manual gain");
    Require(block.timestamp100ns == metadata.timestamp100ns && block.duration100ns == metadata.duration100ns &&
            block.source == metadata.source && block.callGeneration == metadata.callGeneration &&
            block.discontinuity == metadata.discontinuity, "microphone processing changed metadata");

    Require(processor.TrySetNoiseIsolation(false) && processor.TrySetMicrophoneGain(100), "bypass setup failed");
    auto bypass = Block(telestudio::media::AudioTapKind::Microphone, -321);
    const auto before = bypass.samples;
    Require(processor.ProcessMicrophone(bypass) && bypass.samples == before && fake->calls == 1,
            "isolation-off 100-percent path must be bit-identical and skip suppressor");

    Require(processor.TrySetMicrophoneGain(0), "zero gain setup failed");
    auto silence = Block(telestudio::media::AudioTapKind::Microphone, 123);
    Require(processor.ProcessMicrophone(silence) && std::all_of(silence.samples.begin(), silence.samples.end(), [](auto s){return s == 0;}),
            "zero microphone gain must produce silence");

    Require(processor.TrySetMicrophoneGain(150), "saturation setup failed");
    auto positive = Block(telestudio::media::AudioTapKind::Microphone, 30'000);
    auto negative = Block(telestudio::media::AudioTapKind::Microphone, -30'000);
    Require(processor.ProcessMicrophone(positive) && positive.samples.front() == std::numeric_limits<std::int16_t>::max(),
            "positive microphone gain must saturate");
    Require(processor.ProcessMicrophone(negative) && negative.samples.front() == std::numeric_limits<std::int16_t>::min(),
            "negative microphone gain must saturate");
}

void VerifyRoomAndMalformedBlocks() {
    auto fakeOwned = std::make_unique<FakeSuppressor>();
    auto* fake = fakeOwned.get();
    telestudio::media::AudioControlProcessor processor(std::move(fakeOwned));
    auto room = Block(telestudio::media::AudioTapKind::Room, 777);
    const auto roomOriginal = room.samples;
    Require(processor.ApplyRoomPlayback(room) && room.samples == roomOriginal && fake->calls == 0,
            "100-percent room path must be bit-identical and never denoise");
    Require(processor.TrySetRoomMixer({100, 0, 0, 0, false, 30}) && processor.ApplyRoomPlayback(room),
            "independent room noise cleanup failed");
    Require(fake->calls == 0 && processor.ConfirmedState().microphoneGainPercent == 100 &&
            processor.ConfirmedState().noiseIsolation,
            "room noise cleanup changed the microphone DSP state");
    Require(processor.TrySetRoomVolume(0), "room silence setup failed");
    Require(processor.ApplyRoomPlayback(room) && std::all_of(room.samples.begin(), room.samples.end(), [](auto s){return s == 0;}),
            "zero room volume must produce silence");

    for (int malformed = 0; malformed < 5; ++malformed) {
        auto value = Block(telestudio::media::AudioTapKind::Microphone);
        if (malformed == 0) value.channels = 2;
        if (malformed == 1) value.sampleRateHz = 44'100;
        if (malformed == 2) value.samples.pop_back();
        if (malformed == 3) value.duration100ns = 199'999;
        if (malformed == 4) value.source = telestudio::media::AudioTapKind::Room;
        const auto before = value.samples;
        Require(!processor.ProcessMicrophone(value) && value.samples == before, "malformed microphone block was accepted or modified");
    }
    auto malformedRoom = Block(telestudio::media::AudioTapKind::Microphone);
    Require(!processor.ApplyRoomPlayback(malformedRoom), "wrong-source room block was accepted");
    Require(fake->calls == 0, "malformed blocks must not call suppressor");
}

void VerifyFailureAndProductionFactory() {
    auto fakeOwned = std::make_unique<FakeSuppressor>();
    auto* fake = fakeOwned.get();
    fake->succeed = false;
    telestudio::media::AudioControlProcessor processor(std::move(fakeOwned));
    auto block = Block(telestudio::media::AudioTapKind::Microphone, 55);
    Require(processor.TrySetMicrophoneGain(150), "failure gain setup failed");
    Require(processor.ProcessMicrophone(block) && fake->calls == 1 && block.samples.front() == 82,
            "suppressor failure must preserve the microphone block and apply manual gain");
    fake->throwOnFailure = true;
    block = Block(telestudio::media::AudioTapKind::Microphone, 55);
    Require(processor.ProcessMicrophone(block) && block.samples.front() == 82,
            "throwing suppressor discarded or corrupted voice");
    telestudio::media::AudioControlProcessor missing(nullptr);
    block = Block(telestudio::media::AudioTapKind::Microphone, 55);
    Require(missing.ProcessMicrophone(block) && block.samples.front() == 55,
            "missing optional DSP discarded voice");

    auto production = telestudio::media::CreateSpeexNoiseSuppressor();
    Require(static_cast<bool>(production), "Speex suppressor factory returned null");
    Require(!telestudio::media::CreateSpeexNoiseSuppressor(-1) && !telestudio::media::CreateSpeexNoiseSuppressor(31),
            "Speex factory accepted invalid attenuation");
    for (const int rate : {8'000, 16'000, 32'000, 48'000}) {
        std::vector<std::int16_t> synthetic(static_cast<std::size_t>(rate / 50), 0);
        Require(production->Process(synthetic, rate), "Speex suppressor rejected an approved 20-ms format");
    }
}

void VerifyRoomCleanupDoesNotChangeMicrophoneSamples() {
    telestudio::media::AudioControlProcessor baseline, roomCleanup;
    baseline.TrySetNoiseIsolation(false);
    roomCleanup.TrySetNoiseIsolation(false);
    Require(roomCleanup.TrySetRoomMixer({100, 0, 0, 0, false, 30}), "room cleanup setup failed");
    for (const int rate : {8000, 16000, 32000, 48000}) {
        auto room = Block(telestudio::media::AudioTapKind::Room, 2000, rate);
        Require(roomCleanup.ApplyRoomPlayback(room), "room cleanup rejected supported rate");
        auto expected = Block(telestudio::media::AudioTapKind::Microphone, 1234, rate);
        auto actual = expected;
        Require(baseline.ProcessMicrophone(expected) && roomCleanup.ProcessMicrophone(actual) &&
                actual.samples == expected.samples && actual.timestamp100ns == expected.timestamp100ns,
                "room noise reduction changed outgoing microphone samples or timing");
    }
}
}

int main() {
    try {
        VerifyStateAndAtomicSnapshots();
        VerifyRoomMixerFacadePreservesIndependentState();
        VerifyMicrophoneOrderingAndGain();
        VerifyRoomAndMalformedBlocks();
        VerifyFailureAndProductionFactory();
        VerifyRoomCleanupDoesNotChangeMicrophoneSamples();
        std::cout << "audio_control_processor_tests: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "audio_control_processor_tests: " << error.what() << '\n';
        return 1;
    }
}
