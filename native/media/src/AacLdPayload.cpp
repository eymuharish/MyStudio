#include "telestudio/media/AacLdPayload.h"

#include <limits>
#include <stdexcept>

namespace telestudio::media {

std::vector<std::uint8_t> PackRfc3640(
    std::span<const std::uint8_t> accessUnit) {
  constexpr std::size_t kMaximumAuBytes = 0x1fffU;
  if (accessUnit.empty() || accessUnit.size() > kMaximumAuBytes)
    throw std::invalid_argument("RFC 3640 access unit length is unsupported");
  const auto header = static_cast<std::uint16_t>(accessUnit.size() << 3U);
  std::vector<std::uint8_t> result;
  result.reserve(accessUnit.size() + 4U);
  result.push_back(0x00);
  result.push_back(0x10);
  result.push_back(static_cast<std::uint8_t>(header >> 8U));
  result.push_back(static_cast<std::uint8_t>(header));
  result.insert(result.end(), accessUnit.begin(), accessUnit.end());
  return result;
}

std::vector<std::uint8_t> UnpackRfc3640(
    std::span<const std::uint8_t> payload) noexcept {
  if (payload.size() < 4U || payload[0] != 0x00 || payload[1] != 0x10)
    return {};
  const auto header = static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(payload[2]) << 8U) | payload[3]);
  if ((header & 0x7U) != 0U) return {};
  const auto length = static_cast<std::size_t>(header >> 3U);
  if (length == 0U || payload.size() != length + 4U) return {};
  return {payload.begin() + 4, payload.end()};
}

std::vector<std::uint8_t> PackLatm(std::span<const std::uint8_t> accessUnit) {
  if (accessUnit.empty() ||
      accessUnit.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    throw std::invalid_argument("LATM access unit length is unsupported");
  std::vector<std::uint8_t> result;
  result.reserve(accessUnit.size() + accessUnit.size() / 255U + 1U);
  auto remaining = accessUnit.size();
  while (remaining >= 255U) {
    result.push_back(0xff);
    remaining -= 255U;
  }
  result.push_back(static_cast<std::uint8_t>(remaining));
  result.insert(result.end(), accessUnit.begin(), accessUnit.end());
  return result;
}

std::vector<std::uint8_t> UnpackLatm(
    std::span<const std::uint8_t> payload) noexcept {
  if (payload.empty()) return {};
  std::size_t offset = 0;
  std::size_t length = 0;
  while (offset < payload.size()) {
    const auto part = payload[offset++];
    if (length > std::numeric_limits<std::size_t>::max() - part) return {};
    length += part;
    if (part != 0xff) break;
  }
  if (offset == payload.size() || length == 0U ||
      length != payload.size() - offset)
    return {};
  return {payload.begin() + static_cast<std::ptrdiff_t>(offset), payload.end()};
}

}  // namespace telestudio::media
