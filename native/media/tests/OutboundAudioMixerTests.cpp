#include "telestudio/media/OutboundAudioMixer.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using telestudio::media::AudioTapKind;
using telestudio::media::OwnedAudioBlock;

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

OwnedAudioBlock Tone(AudioTapKind source,
                     std::int16_t value,
                     std::int64_t timestamp100ns,
                     bool discontinuity = false) {
    return {std::vector<std::int16_t>(960, value), 48'000, 1, timestamp100ns, 200'000,
            source, 7, discontinuity};
}

template <typename Samples>
bool AllSamples(const Samples& samples, std::int16_t value) {
    return std::all_of(samples.begin(), samples.end(), [value](std::int16_t sample) {
        return sample == value;
    });
}

void VerifySaturationPreservesPositiveFullScale() {
    telestudio::media::OutboundAudioMixer mixer;
    mixer.PushMicrophone(Tone(AudioTapKind::Microphone, 32'767, 1'500'000));
    mixer.PushSystemAudio(Tone(AudioTapKind::SystemAudio, 32'767, 1'500'000));
    Require(AllSamples(mixer.TakeMixedBlock().samples, 32'767),
            "full-scale aligned sources must not overflow int16 during headroom mixing");
}

void VerifyAlignedMixUsesHeadroom() {
    telestudio::media::OutboundAudioMixer mixer;
    mixer.PushMicrophone(Tone(AudioTapKind::Microphone, 1'000, 1'000'000));
    mixer.PushSystemAudio(Tone(AudioTapKind::SystemAudio, 2'000, 1'000'000));

    const auto mixed = mixer.TakeMixedBlock();
    Require(mixed.samples.size() == 960 && AllSamples(mixed.samples, 1'500),
            "aligned microphone and system audio must mix with 6-dB headroom");
    Require(mixed.sampleRateHz == 48'000 && mixed.channels == 1 &&
                mixed.timestamp100ns == 1'000'000 && mixed.duration100ns == 200'000,
            "mixed audio must retain the 48-kHz mono 20-ms timing contract");
}

void VerifyRoomAudioCannotEnterOutboundSystemPath() {
    telestudio::media::OutboundAudioMixer mixer;
    mixer.PushMicrophone(Tone(AudioTapKind::Microphone, 1'000, 1'750'000));
    mixer.PushSystemAudio(Tone(AudioTapKind::Room, 2'000, 1'750'000));
    Require(AllSamples(mixer.TakeMixedBlock().samples, 1'000),
            "received room audio must not enter the outbound system-audio path");
}

void VerifyInitialSilenceDoesNotLockMixerEpoch() {
    telestudio::media::OutboundAudioMixer mixer;
    mixer.SetMicrophoneMuted(true);
    Require(AllSamples(mixer.TakeMixedBlock().samples, 0), "initial muted pull must be silent");

    mixer.SetMicrophoneMuted(false);
    mixer.PushMicrophone(Tone(AudioTapKind::Microphone, 1'300, 7'000'000));
    const auto resumed = mixer.TakeMixedBlock();
    Require(AllSamples(resumed.samples, 1'300) && resumed.timestamp100ns == 7'000'000,
            "initial silence must not lock later audio to a zero-based timeline");
}

void VerifySkewedSourceOriginsMixInOneSlot() {
    telestudio::media::OutboundAudioMixer mixer;
    mixer.PushMicrophone(Tone(AudioTapKind::Microphone, 1'000, 3'000'000));
    mixer.PushSystemAudio(Tone(AudioTapKind::SystemAudio, 2'000, 3'073'000));
    const auto mixed = mixer.TakeMixedBlock();
    Require(AllSamples(mixed.samples, 1'500) && mixed.timestamp100ns == 3'000'000,
            "independently started sources must normalize their first frames into one mixer slot");
}

void VerifyLateSourceJoinsCurrentSlotAfterMissingInput() {
    telestudio::media::OutboundAudioMixer mixer;
    mixer.PushMicrophone(Tone(AudioTapKind::Microphone, 1'000, 4'000'000));
    Require(AllSamples(mixer.TakeMixedBlock().samples, 1'000), "initial microphone frame was not emitted");
    Require(AllSamples(mixer.TakeMixedBlock().samples, 0), "missing input must zero-fill its mixer slot");

    mixer.PushSystemAudio(Tone(AudioTapKind::SystemAudio, 2'000, 9'000'000));
    const auto lateSystem = mixer.TakeMixedBlock();
    Require(AllSamples(lateSystem.samples, 2'000) && lateSystem.timestamp100ns == 4'400'000,
            "late system audio must join the current mixer slot instead of being discarded by raw timestamp");
}

void VerifyDiscontinuityRebasesSourceAtNextSlot() {
    telestudio::media::OutboundAudioMixer mixer;
    mixer.PushMicrophone(Tone(AudioTapKind::Microphone, 1'000, 5'000'000));
    mixer.PushSystemAudio(Tone(AudioTapKind::SystemAudio, 2'000, 5'000'000));
    Require(AllSamples(mixer.TakeMixedBlock().samples, 1'500), "discontinuity setup frame was not mixed");

    mixer.PushMicrophone(Tone(AudioTapKind::Microphone, 1'000, 19'000'000, true));
    mixer.PushSystemAudio(Tone(AudioTapKind::SystemAudio, 2'000, 5'200'000));
    const auto recovered = mixer.TakeMixedBlock();
    Require(AllSamples(recovered.samples, 1'500) && recovered.discontinuity &&
                recovered.timestamp100ns == 5'200'000,
            "source discontinuity must rebase at the next mixer slot without losing its peer");
}

void VerifyConcurrentCallbacksControlsAndPullsStayWellFormed() {
    telestudio::media::OutboundAudioMixer mixer;
    std::atomic<bool> start = false;
    std::atomic<bool> malformed = false;
    const auto waitForStart = [&start] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
    };
    const auto checkOutput = [&mixer, &malformed, &waitForStart] {
        waitForStart();
        for (int index = 0; index < 256; ++index) {
            const auto block = mixer.TakeMixedBlock();
            if (block.samples.size() != 960 || block.sampleRateHz != 48'000 || block.channels != 1 ||
                block.duration100ns != 200'000) {
                malformed.store(true, std::memory_order_release);
            }
        }
    };

    std::thread microphone([&] {
        waitForStart();
        for (int index = 0; index < 128; ++index) {
            mixer.PushMicrophone(Tone(AudioTapKind::Microphone, 1'000,
                                      10'000'000 + static_cast<std::int64_t>(index) * 200'000));
        }
    });
    std::thread systemAudio([&] {
        waitForStart();
        for (int index = 0; index < 128; ++index) {
            mixer.PushSystemAudio(Tone(AudioTapKind::SystemAudio, 2'000,
                                       16'000'000 + static_cast<std::int64_t>(index) * 200'000));
        }
    });
    std::thread controls([&] {
        waitForStart();
        for (int index = 0; index < 128; ++index) {
            mixer.SetMicrophoneMuted((index % 2) == 0);
            mixer.SetSystemAudioMuted((index % 3) == 0);
        }
        mixer.SetMicrophoneMuted(false);
        mixer.SetSystemAudioMuted(false);
    });
    std::thread pull(checkOutput);

    start.store(true, std::memory_order_release);
    microphone.join();
    systemAudio.join();
    controls.join();
    pull.join();
    Require(!malformed.load(std::memory_order_acquire),
            "concurrent callbacks, controls, and pulls must preserve a well-formed outbound block");
}

void VerifyIndependentMutesAndTimedSilence() {
    telestudio::media::OutboundAudioMixer mixer;
    mixer.SetMicrophoneMuted(true);
    mixer.PushMicrophone(Tone(AudioTapKind::Microphone, 1'000, 2'000'000));
    mixer.PushSystemAudio(Tone(AudioTapKind::SystemAudio, 2'000, 2'000'000));
    Require(AllSamples(mixer.TakeMixedBlock().samples, 2'000),
            "microphone mute must leave system audio audible");

    mixer.SetSystemAudioMuted(true);
    const auto silent = mixer.TakeMixedBlock();
    Require(AllSamples(silent.samples, 0) && silent.timestamp100ns == 2'200'000 &&
                silent.duration100ns == 200'000,
            "both mutes must emit the next correctly timed silent block");

    mixer.SetMicrophoneMuted(false);
    mixer.PushMicrophone(Tone(AudioTapKind::Microphone, 1'200, 2'400'000));
    Require(AllSamples(mixer.TakeMixedBlock().samples, 1'200),
            "system-audio mute must leave microphone audio audible");
}
}

int main() {
    try {
        {
          telestudio::media::OutboundAudioMixer buffered;
          Require(buffered.SetSendBufferMs(100), "100ms buffer accepted before audio");
          for(int slot=0;slot<10;++slot) {
            buffered.PushMicrophone(Tone(AudioTapKind::Microphone,1200,slot*200000));
            const auto block=buffered.TakeMixedBlock();
            Require(AllSamples(block.samples,slot<4 ? 0 : 1200), "prebuffer then continuous output without periodic gaps");
          }
          Require(!buffered.SetSendBufferMs(200), "active buffer change requires new session");
        }
        VerifyAlignedMixUsesHeadroom();
        VerifySaturationPreservesPositiveFullScale();
        VerifyRoomAudioCannotEnterOutboundSystemPath();
        VerifyInitialSilenceDoesNotLockMixerEpoch();
        VerifySkewedSourceOriginsMixInOneSlot();
        VerifyLateSourceJoinsCurrentSlotAfterMissingInput();
        VerifyDiscontinuityRebasesSourceAtNextSlot();
        VerifyConcurrentCallbacksControlsAndPullsStayWellFormed();
        VerifyIndependentMutesAndTimedSilence();
        std::cout << "outbound_audio_mixer_tests: PASS\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "outbound_audio_mixer_tests: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
