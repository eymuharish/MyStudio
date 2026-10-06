#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace telestudio::media {

enum class AacLdTransport {
  Raw,
  Latm,
};

struct AacLdConfiguration {
  int sampleRate{48000};
  int channels{1};
  int bitrate{64000};
  int audioObjectType{23};
  AacLdTransport transport{AacLdTransport::Raw};
  // Zero keeps the codec's native frame length. Cisco H.323 AAC-LD uses
  // two 480-sample subframes in one LATM transport; SIP keeps 512/1.
  int granuleLength{0};
  int transportSubframes{1};
};

struct DecodedAacAudio {
  std::vector<std::int16_t> pcm;
  int sampleRate{0};
  int channels{0};
  int frameSize{0};
};

[[nodiscard]] bool ValidateAacLd(const AacLdConfiguration& configuration) noexcept;
[[nodiscard]] bool ValidateMpeg4Aac(
    const AacLdConfiguration& configuration) noexcept;

class Mpeg4AacEncoder final {
 public:
  explicit Mpeg4AacEncoder(AacLdConfiguration configuration);
  ~Mpeg4AacEncoder();

  Mpeg4AacEncoder(const Mpeg4AacEncoder&) = delete;
  Mpeg4AacEncoder& operator=(const Mpeg4AacEncoder&) = delete;
  Mpeg4AacEncoder(Mpeg4AacEncoder&&) noexcept;
  Mpeg4AacEncoder& operator=(Mpeg4AacEncoder&&) noexcept;

  [[nodiscard]] const AacLdConfiguration& Configuration() const noexcept;
  [[nodiscard]] const std::vector<std::uint8_t>& AudioSpecificConfig() const noexcept;
  [[nodiscard]] std::vector<std::uint8_t> Encode(std::span<const std::int16_t> pcm);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class Mpeg4AacDecoder final {
 public:
  Mpeg4AacDecoder(AacLdConfiguration configuration,
                  std::span<const std::uint8_t> audioSpecificConfig);
  ~Mpeg4AacDecoder();

  Mpeg4AacDecoder(const Mpeg4AacDecoder&) = delete;
  Mpeg4AacDecoder& operator=(const Mpeg4AacDecoder&) = delete;
  Mpeg4AacDecoder(Mpeg4AacDecoder&&) noexcept;
  Mpeg4AacDecoder& operator=(Mpeg4AacDecoder&&) noexcept;

  [[nodiscard]] std::vector<std::int16_t> Decode(
      std::span<const std::uint8_t> accessUnit);
  [[nodiscard]] DecodedAacAudio DecodeDetailed(
      std::span<const std::uint8_t> accessUnit);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

using AacLdEncoder = Mpeg4AacEncoder;
using AacLdDecoder = Mpeg4AacDecoder;

}  // namespace telestudio::media
