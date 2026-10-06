#include "telestudio/media/MonotonicClock.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstdint>
#include <limits>

namespace telestudio::media {
namespace {
constexpr std::uint64_t kTicksPerSecond100ns = 10'000'000;

std::uint64_t MultiplyRemainderAndDivide(std::uint64_t value,
                                         std::uint64_t multiplier,
                                         std::uint64_t divisor) noexcept {
    std::uint64_t quotient = 0;
    std::uint64_t remainder = 0;
    bool started = false;
    for (int bit = 63; bit >= 0; --bit) {
        const bool set = ((multiplier >> bit) & 1U) != 0;
        if (!started && !set) continue;
        started = true;
        quotient <<= 1U;
        if (remainder >= divisor - remainder) {
            remainder -= divisor - remainder;
            ++quotient;
        } else {
            remainder += remainder;
        }
        if (set) {
            if (remainder >= divisor - value) {
                remainder -= divisor - value;
                ++quotient;
            } else {
                remainder += value;
            }
        }
    }
    return quotient;
}
}

TickConversionResult ConvertMonotonicTicksTo100ns(std::int64_t counter,
                                                   std::int64_t frequency) noexcept {
    if (counter < 0 || frequency <= 0) return {};
    const auto unsignedCounter = static_cast<std::uint64_t>(counter);
    const auto unsignedFrequency = static_cast<std::uint64_t>(frequency);
    const auto seconds = unsignedCounter / unsignedFrequency;
    const auto remainder = unsignedCounter % unsignedFrequency;
    constexpr auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (seconds > maximum / kTicksPerSecond100ns) {
        return {std::numeric_limits<std::int64_t>::max(), true, true};
    }
    const auto whole = seconds * kTicksPerSecond100ns;
    const auto fraction = MultiplyRemainderAndDivide(remainder, kTicksPerSecond100ns, unsignedFrequency);
    if (fraction > maximum - whole) {
        return {std::numeric_limits<std::int64_t>::max(), true, true};
    }
    return {static_cast<std::int64_t>(whole + fraction), true, false};
}

std::int64_t QpcMonotonicClock::Now100ns() noexcept {
    static const std::int64_t frequency = []() noexcept {
        LARGE_INTEGER value{};
        return QueryPerformanceFrequency(&value) ? value.QuadPart : 0;
    }();
    LARGE_INTEGER counter{};
    if (frequency <= 0 || !QueryPerformanceCounter(&counter)) return 0;
    return ConvertMonotonicTicksTo100ns(counter.QuadPart, frequency).value100ns;
}

} // namespace telestudio::media
