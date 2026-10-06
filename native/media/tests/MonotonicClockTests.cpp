#include "telestudio/media/MonotonicClock.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
}

int main() {
    try {
        using telestudio::media::ConvertMonotonicTicksTo100ns;
        auto exact = ConvertMonotonicTicksTo100ns(15, 3);
        Require(exact.valid && !exact.saturated && exact.value100ns == 50'000'000, "exact integer conversion failed");

        auto fractional = ConvertMonotonicTicksTo100ns(1, 3);
        Require(fractional.valid && fractional.value100ns == 3'333'333, "non-divisible conversion must floor exactly");

        auto nearLimit = ConvertMonotonicTicksTo100ns(std::numeric_limits<std::int64_t>::max(),
                                                      std::numeric_limits<std::int64_t>::max());
        Require(nearLimit.valid && !nearLimit.saturated && nearLimit.value100ns == 10'000'000,
                "large counters must convert without intermediate overflow");

        auto saturated = ConvertMonotonicTicksTo100ns(std::numeric_limits<std::int64_t>::max(), 1);
        Require(saturated.valid && saturated.saturated &&
                saturated.value100ns == std::numeric_limits<std::int64_t>::max(), "overflow must saturate");
        Require(!ConvertMonotonicTicksTo100ns(1, 0).valid, "zero frequency must be rejected");
        Require(!ConvertMonotonicTicksTo100ns(-1, 10).valid, "negative counter must be rejected");

        auto first = ConvertMonotonicTicksTo100ns(9'999, 7'777);
        auto second = ConvertMonotonicTicksTo100ns(10'000, 7'777);
        Require(first.valid && second.valid && second.value100ns >= first.value100ns, "conversion must be monotonic");

        telestudio::media::QpcMonotonicClock clockA;
        telestudio::media::QpcMonotonicClock clockB;
        const auto a = clockA.Now100ns();
        const auto b = clockB.Now100ns();
        Require(a > 0 && b >= a, "QPC clocks must share one process-wide epoch rather than reset per instance");
        std::cout << "monotonic_clock_tests: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "monotonic_clock_tests: " << error.what() << '\n';
        return 1;
    }
}
