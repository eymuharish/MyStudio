#pragma once

#include <cstdint>

namespace telestudio::detail {

class EncoderPtsSequence final {
public:
    std::int64_t Next() noexcept {
        return next_++;
    }

    void Reset() noexcept {
        next_ = 0;
    }

private:
    std::int64_t next_ = 0;
};

} // namespace telestudio::detail
