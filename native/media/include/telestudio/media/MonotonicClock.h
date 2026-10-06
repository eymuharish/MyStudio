#pragma once

#include <cstdint>

namespace telestudio::media {

struct TickConversionResult {
    std::int64_t value100ns = 0;
    bool valid = false;
    bool saturated = false;
};

TickConversionResult ConvertMonotonicTicksTo100ns(std::int64_t counter,
                                                   std::int64_t frequency) noexcept;

class IMonotonicClock {
public:
    virtual ~IMonotonicClock() = default;
    virtual std::int64_t Now100ns() noexcept = 0;
};

class QpcMonotonicClock final : public IMonotonicClock {
public:
    std::int64_t Now100ns() noexcept override;
};

} // namespace telestudio::media
