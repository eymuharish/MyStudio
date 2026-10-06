#include "telestudio/media/AudioBlockFramer.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <initializer_list>
#include <stdexcept>
#include <vector>

namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

class FakeClock final : public telestudio::media::IMonotonicClock {
public:
    FakeClock(std::initializer_list<std::int64_t> observations) : values(observations) {}
    std::vector<std::int64_t> values;
    std::size_t reads = 0;
    std::int64_t Now100ns() noexcept override {
        const auto index = std::min(reads, values.size() - 1);
        ++reads;
        return values[index];
    }
};

std::vector<std::int16_t> Samples(std::size_t count, std::int16_t start = 1) {
    std::vector<std::int16_t> result(count);
    for (std::size_t i = 0; i < count; ++i) result[i] = static_cast<std::int16_t>(start + i);
    return result;
}

void VerifySupportedRatesAndOwnership() {
    for (const int rate : {8'000, 16'000, 32'000, 48'000}) {
        FakeClock clock{{1'000'000 + rate}};
        telestudio::media::AudioBlockFramer framer(clock);
        auto input = Samples(static_cast<std::size_t>(rate / 50));
        const auto expected = input;
        auto blocks = framer.Push(input, rate, telestudio::media::AudioTapKind::Microphone, 7);
        std::fill(input.begin(), input.end(), -9);
        Require(blocks.size() == 1 && blocks[0].samples == expected, "supported-rate block must own callback samples");
        Require(blocks[0].sampleRateHz == rate && blocks[0].channels == 1, "framed block format is wrong");
        Require(blocks[0].timestamp100ns == 1'000'000 + rate && blocks[0].duration100ns == 200'000,
                "framed timestamp/duration is wrong");
        Require(!blocks[0].discontinuity && blocks[0].callGeneration == 7, "initial block metadata is wrong");
        Require(clock.reads == 1, "accepted callback must read the clock exactly once");
    }
}

void VerifySplitCoalesceAndTimeline() {
    FakeClock clock{{2'000'000, 2'100'000, 2'100'000, 2'100'000}};
    telestudio::media::AudioBlockFramer framer(clock);
    auto firstHalf = Samples(480, 10);
    auto secondHalf = Samples(480, 500);
    Require(framer.Push(firstHalf, 48'000, telestudio::media::AudioTapKind::Room, 2).empty(), "half block must coalesce");
    auto coalesced = framer.Push(secondHalf, 48'000, telestudio::media::AudioTapKind::Room, 2);
    Require(coalesced.size() == 1 && coalesced[0].samples.front() == 10 && coalesced[0].samples[480] == 500,
            "coalesced block contents are wrong");
    Require(coalesced[0].timestamp100ns == 2'000'000, "coalesced block must retain first callback timestamp");

    auto two = Samples(1'920, 20);
    auto split = framer.Push(two, 48'000, telestudio::media::AudioTapKind::Room, 2);
    Require(split.size() == 2, "large callback must split into two blocks");
    Require(split[0].timestamp100ns == 2'200'000 && split[1].timestamp100ns == 2'400'000,
            "block timestamps must advance from accepted sample counts");
    Require(split[0].duration100ns == 200'000 && split[1].duration100ns == 200'000,
            "split blocks require exact 20-ms durations");
    Require(clock.reads == 3, "multiple blocks from one callback must still use one clock read");

    auto oneSample = Samples(1, 77);
    Require(framer.Push(oneSample, 48'000, telestudio::media::AudioTapKind::Room, 2).empty(), "tail must remain incomplete");
}

void VerifyResetAndGenerationRules() {
    FakeClock clock{{3'000'000, 3'100'000, 3'200'000, 3'300'000, 3'400'000, 3'500'000, 3'600'000}};
    telestudio::media::AudioBlockFramer framer(clock);
    auto half8 = Samples(80);
    Require(framer.Push(half8, 8'000, telestudio::media::AudioTapKind::Microphone, 4).empty(), "tail setup failed");

    auto full16 = Samples(320, 100);
    auto formatReset = framer.Push(full16, 16'000, telestudio::media::AudioTapKind::Microphone, 4);
    Require(formatReset.size() == 1 && formatReset[0].discontinuity && formatReset[0].timestamp100ns == 3'100'000,
            "format change must discard tail and mark discontinuity");

    auto sourceReset = framer.Push(full16, 16'000, telestudio::media::AudioTapKind::Room, 4);
    Require(sourceReset.size() == 1 && sourceReset[0].discontinuity, "source change must mark discontinuity");

    auto generationReset = framer.Push(full16, 16'000, telestudio::media::AudioTapKind::Room, 5);
    Require(generationReset.size() == 1 && generationReset[0].discontinuity && generationReset[0].callGeneration == 5,
            "higher generation must restart discontinuously");

    const auto readsBeforeStale = clock.reads;
    auto stale = framer.Push(full16, 16'000, telestudio::media::AudioTapKind::Room, 4);
    Require(stale.empty() && clock.reads == readsBeforeStale, "stale generation must not read clock or mutate state");

    framer.Reset();
    auto explicitReset = framer.Push(full16, 16'000, telestudio::media::AudioTapKind::Room, 5);
    Require(explicitReset.size() == 1 && explicitReset[0].discontinuity, "explicit reset must mark next block discontinuous");
}

void VerifyClockDiscontinuitiesAndRejections() {
    FakeClock gapClock{{4'000'000, 4'300'001}};
    telestudio::media::AudioBlockFramer gapFramer(gapClock);
    auto half = Samples(480);
    Require(gapFramer.Push(half, 48'000, telestudio::media::AudioTapKind::Microphone, 1).empty(), "gap setup failed");
    auto full = Samples(960, 100);
    auto gap = gapFramer.Push(full, 48'000, telestudio::media::AudioTapKind::Microphone, 1);
    Require(gap.size() == 1 && gap[0].discontinuity && gap[0].timestamp100ns == 4'300'001,
            "clock more than one block ahead must restart timeline");

    FakeClock backwardsClock{{5'000'000, 4'900'000}};
    telestudio::media::AudioBlockFramer backwards(backwardsClock);
    Require(backwards.Push(half, 48'000, telestudio::media::AudioTapKind::Room, 1).empty(), "backward setup failed");
    auto backward = backwards.Push(full, 48'000, telestudio::media::AudioTapKind::Room, 1);
    Require(backward.size() == 1 && backward[0].discontinuity && backward[0].timestamp100ns == 4'900'000,
            "backward observation must restart timeline");

    FakeClock rejectedClock{{6'000'000}};
    telestudio::media::AudioBlockFramer rejected(rejectedClock);
    auto unsupported = rejected.Push(full, 44'100, telestudio::media::AudioTapKind::Room, 1);
    Require(unsupported.empty() && rejectedClock.reads == 0, "unsupported rate must be rejected without a clock read");
    auto empty = rejected.Push({}, 48'000, telestudio::media::AudioTapKind::Room, 1);
    Require(empty.empty() && rejectedClock.reads == 0, "empty callback must not read clock or retain data");

    FakeClock tailClock{{7'000'000, 7'100'000}};
    telestudio::media::AudioBlockFramer tailFramer(tailClock);
    auto firstHalf8 = Samples(80, 111);
    Require(tailFramer.Push(firstHalf8, 8'000, telestudio::media::AudioTapKind::Microphone, 2).empty(),
            "tail preservation setup failed");
    const auto readsWithTail = tailClock.reads;
    Require(tailFramer.Push(full, 44'100, telestudio::media::AudioTapKind::Microphone, 2).empty() &&
            tailFramer.Push(full, 8'000, telestudio::media::AudioTapKind::Microphone, 1).empty() &&
            tailClock.reads == readsWithTail, "unsupported/stale callbacks must not leak samples or mutate the pending timeline");
    auto secondHalf8 = Samples(80, 500);
    auto completed = tailFramer.Push(secondHalf8, 8'000, telestudio::media::AudioTapKind::Microphone, 2);
    Require(completed.size() == 1 && completed[0].samples.front() == 111 && completed[0].samples[80] == 500 &&
            completed[0].timestamp100ns == 7'000'000 && !completed[0].discontinuity,
            "rejected callbacks must leave the owned incomplete tail unchanged");
}
}

int main() {
    try {
        VerifySupportedRatesAndOwnership();
        VerifySplitCoalesceAndTimeline();
        VerifyResetAndGenerationRules();
        VerifyClockDiscontinuitiesAndRejections();
        std::cout << "audio_block_framer_tests: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "audio_block_framer_tests: " << error.what() << '\n';
        return 1;
    }
}
