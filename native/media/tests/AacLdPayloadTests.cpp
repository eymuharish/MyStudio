#include "telestudio/media/AacLdPayload.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
}  // namespace

int main() {
  using namespace telestudio::media;
  const std::array<std::uint8_t, 3> accessUnit{0x11, 0x22, 0x33};
  const std::vector<std::uint8_t> rfcExpected{
      0x00, 0x10, 0x00, 0x18, 0x11, 0x22, 0x33};
  Require(PackRfc3640(accessUnit) == rfcExpected,
          "RFC 3640 payload must contain a 16-bit AU header");
  Require(UnpackRfc3640(rfcExpected) ==
              std::vector<std::uint8_t>(accessUnit.begin(), accessUnit.end()),
          "RFC 3640 payload must round-trip");
  const std::array<std::uint8_t, 3> truncatedRfc{0x00, 0x10, 0x00};
  Require(UnpackRfc3640(truncatedRfc).empty(),
          "RFC 3640 must reject truncated payloads");
  const std::array<std::uint8_t, 5> multipleRfc{0x00, 0x18, 0x00, 0x18,
                                               0x00};
  Require(UnpackRfc3640(multipleRfc).empty(),
          "RFC 3640 must reject multiple AU headers");

  const std::vector<std::uint8_t> latmExpected{0x03, 0x11, 0x22, 0x33};
  Require(PackLatm(accessUnit) == latmExpected,
          "LATM payload must contain one payload-length field");
  Require(UnpackLatm(latmExpected) ==
              std::vector<std::uint8_t>(accessUnit.begin(), accessUnit.end()),
          "LATM payload must round-trip");
  const std::array<std::uint8_t, 4> invalidLatm{0x04, 0x11, 0x22, 0x33};
  Require(UnpackLatm(invalidLatm).empty(),
          "LATM must reject inconsistent payload lengths");

  std::vector<std::uint8_t> longAccessUnit(300U, 0x5a);
  const auto longLatm = PackLatm(longAccessUnit);
  Require(longLatm.size() == 302U && longLatm[0] == 0xff &&
              longLatm[1] == 45 && UnpackLatm(longLatm) == longAccessUnit,
          "LATM must support continuation length bytes");

  std::cout << "AAC-LD payload cases passed\n";
  return 0;
}
