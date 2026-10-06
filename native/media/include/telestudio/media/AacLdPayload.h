#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace telestudio::media {

[[nodiscard]] std::vector<std::uint8_t> PackRfc3640(
    std::span<const std::uint8_t> accessUnit);
[[nodiscard]] std::vector<std::uint8_t> UnpackRfc3640(
    std::span<const std::uint8_t> payload) noexcept;
[[nodiscard]] std::vector<std::uint8_t> PackLatm(
    std::span<const std::uint8_t> accessUnit);
[[nodiscard]] std::vector<std::uint8_t> UnpackLatm(
    std::span<const std::uint8_t> payload) noexcept;

}  // namespace telestudio::media
