#include "telestudio/media/RecordingAudioMixer.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
telestudio::media::OwnedAudioBlock Block(telestudio::media::AudioTapKind source,
                                         int rate, std::int16_t sample, std::int64_t time) {
    return {std::vector<std::int16_t>(static_cast<std::size_t>(rate / 50), sample),
            rate, 1, time, 200'000, source, 7, false};
}
}

int main() {
    try {
        using namespace telestudio;
        using telestudio::media::AudioTapKind;
        for (const int rate : {8000, 16000, 32000, 48000}) {
            RecordingAudioMixer mixer(true, false, 7, 1'000'000);
            const auto result = mixer.Push(Block(AudioTapKind::Microphone, rate, 1000, 900'000));
            Require(result.status == AudioMixStatus::Accepted && result.blocks.size() == 1,
                    "single requested source must produce one block");
            Require(result.blocks[0].samples.size() == 960 && result.blocks[0].timestamp100ns == 0 &&
                    result.blocks[0].duration100ns == 200'000,
                    "all supported rates must become exact 48 kHz 20 ms blocks");
        }
        RecordingAudioMixer mixed(true, true, 7, 0);
        Require(mixed.Push(Block(AudioTapKind::Microphone, 48000, 32767, 0)).blocks.empty(),
                "two-source mixer must align before output");
        const auto pair = mixed.Push(Block(AudioTapKind::Room, 48000, 32767, 0));
        Require(pair.blocks.size() == 1 && pair.blocks[0].samples.front() == 29204,
                "two-source headroom must feed the exact -1 dBFS limiter");
        Require(pair.releasedInputTimestamps100ns == std::vector<std::int64_t>{0},
                "a paired timestamp must release exactly one aggregate admission unit");
        RecordingAudioMixer realClockPair(true, true, 7, 1'000'000);
        Require(realClockPair.Push(Block(AudioTapKind::Microphone, 48000, 1000, 1'010'000)).blocks.empty(),
                "the first real-clock source must wait for its peer");
        const auto normalizedPair =
            realClockPair.Push(Block(AudioTapKind::Room, 48000, 1000, 1'018'000));
        Require(normalizedPair.status == AudioMixStatus::Accepted &&
                    normalizedPair.blocks.size() == 1 &&
                    normalizedPair.blocks[0].timestamp100ns == 0,
                "independent callback clocks in one 20 ms bucket must align to the recording epoch");
        Require(normalizedPair.releasedInputTimestamps100ns ==
                    std::vector<std::int64_t>({1'010'000, 1'018'000}),
                "aligned callback clocks must release both original admission timestamps");
        for (int index = 1; index <= 300; ++index) {
            const auto micTimestamp = 1'010'000 + index * 200'000ll;
            const auto roomTimestamp = 1'018'000 + index * 200'000ll;
            Require(realClockPair.Push(Block(AudioTapKind::Microphone, 48000, 1000,
                                             micTimestamp)).blocks.empty(),
                    "sustained independent microphone clocks must wait for their peer");
            const auto sustainedPair = realClockPair.Push(
                Block(AudioTapKind::Room, 48000, 1000, roomTimestamp));
            Require(sustainedPair.status == AudioMixStatus::Accepted &&
                        sustainedPair.blocks.size() == 1 &&
                        sustainedPair.releasedInputTimestamps100ns ==
                            std::vector<std::int64_t>({micTimestamp, roomTimestamp}),
                    "sustained independent clocks must release every raw admission key");
        }
        RecordingAudioMixer aligned(true, true, 7, 0);
        Require(aligned.Push(Block(AudioTapKind::Microphone, 48000, 2000, 0)).blocks.empty(),
                "alignment waits while the peer timestamp is unknown");
        const auto gap = aligned.Push(Block(AudioTapKind::Room, 48000, 2000, 200'000));
        Require(gap.blocks.size() == 1 && gap.blocks[0].timestamp100ns == 0 &&
                    gap.blocks[0].samples.front() == 1000,
                "an observed QPC gap must insert aligned silence with two-source headroom");
        RecordingAudioMixer epoch(true, false, 7, 1'000'000);
        Require(epoch.Push(Block(AudioTapKind::Microphone, 48000, 1, 900'000)).blocks[0].timestamp100ns == 0,
                "only the first pre-epoch sample may clamp to zero");
        Require(epoch.Push(Block(AudioTapKind::Microphone, 48000, 1, 950'000)).status == AudioMixStatus::TimingLost,
                "a later pre-epoch timestamp must fail rather than collapse onto zero");
        RecordingAudioMixer videoOnly(false, false, 7, 0);
        Require(videoOnly.Push(Block(AudioTapKind::Room, 48000, 1, 0)).status == AudioMixStatus::Ignored,
                "video-only mode must reject every audio source");
        auto malformed = Block(AudioTapKind::Microphone, 48000, 1, 0);
        malformed.samples.pop_back();
        Require(mixed.Push(std::move(malformed)).status == AudioMixStatus::Malformed,
                "malformed blocks must fail closed");
        RecordingAudioMixer roomOnly(false, true, 7, 0);
        Require(roomOnly.Push(Block(AudioTapKind::Room, 16000, -1200, 0)).blocks.size() == 1,
                "room-only protocol receive audio must be supported");
        RecordingAudioMixer loss(true, true, 7, 0);
        loss.Push(Block(AudioTapKind::Room, 48000, 2000, 0));
        const auto lossDrain = loss.MarkUnavailable(AudioTapKind::Microphone);
        Require(lossDrain.warning && lossDrain.blocks.size() == 1 &&
                    lossDrain.blocks[0].samples.front() == 1000,
                "tap loss must warn and drain the remaining source with bounded silence headroom");
        AudioMixResult continued;
        for (int index = 1; index <= 26; ++index)
            continued = loss.Push(Block(AudioTapKind::Room, 48000, 2000, index * 200'000ll));
        Require(continued.status == AudioMixStatus::Accepted && !continued.blocks.empty() &&
                    continued.blocks.back().samples.front() == 2000,
                "after 500 ms concealment the remaining source must continue at one-source level");
        RecordingAudioMixer delayedPeer(true, true, 7, 0);
        AudioMixResult delayedResult;
        for (int index = 0; index < 75; ++index) {
            delayedResult = delayedPeer.Push(
                Block(AudioTapKind::Microphone, 48000, 1800, index * 200'000ll));
            Require(delayedResult.status == AudioMixStatus::Accepted && delayedPeer.PendingBlocks() <= 25,
                    "a temporarily delayed peer must not overflow the bounded recording queue");
        }
        Require(delayedResult.warning && !delayedResult.blocks.empty() &&
                    delayedResult.blocks.back().discontinuity,
                "a delayed peer must degrade to bounded one-source audio with an explicit warning");
        RecordingAudioMixer stopDrain(true, true, 7, 0);
        stopDrain.Push(Block(AudioTapKind::Microphone, 48000, 2400, 0));
        stopDrain.Push(Block(AudioTapKind::Microphone, 48000, 2400, 200'000));
        Require(stopDrain.MarkUnavailable(AudioTapKind::Microphone).blocks.empty(),
                "stop drain may need both requested taps resolved before output");
        const auto stoppedDrain = stopDrain.MarkUnavailable(AudioTapKind::Room);
        Require(stoppedDrain.blocks.size() == 2 && stoppedDrain.blocks[0].samples.front() == 1200,
                "Stop must drain accepted unmatched mixer audio with bounded missing-peer silence");
        Require(stoppedDrain.releasedInputTimestamps100ns ==
                    std::vector<std::int64_t>({0, 200'000}),
                "Stop drain must release every accepted unmatched aggregate timestamp");
        RecordingAudioMixer duplicate(true, false, 7, 0);
        Require(duplicate.Push(Block(AudioTapKind::Microphone, 48000, 1, 0)).status == AudioMixStatus::Accepted,
                "first timeline block must be accepted");
        Require(duplicate.Push(Block(AudioTapKind::Microphone, 48000, 1, 0)).status == AudioMixStatus::TimingLost,
                "duplicate/overlapping timestamps must fail closed");
        RecordingAudioMixer boundedGap(true, false, 7, 0);
        boundedGap.Push(Block(AudioTapKind::Microphone, 48000, 1, 0));
        const auto concealed = boundedGap.Push(Block(AudioTapKind::Microphone, 48000, 1, 400'000));
        Require(concealed.blocks.size() == 2 && concealed.blocks[0].samples.front() == 0 &&
                    concealed.blocks[0].discontinuity,
                "one missing timeline block must be explicit bounded silence");
        Require(boundedGap.Push(Block(AudioTapKind::Microphone, 48000, 1, 6'000'000)).status ==
                    AudioMixStatus::TimingLost,
                "a gap beyond 500 ms must stop rather than synthesize unlimited silence");
        std::cout << "recording_audio_mixer_tests: PASS\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "recording_audio_mixer_tests: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
