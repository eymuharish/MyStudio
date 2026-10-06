#include "telestudio/media/AacLdCodec.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

std::vector<std::int16_t> StereoFixture(std::size_t frames = 512U) {
  std::vector<std::int16_t> result(frames * 2U);
  for (std::size_t frame = 0; frame < frames; ++frame) {
    const auto left = static_cast<std::int16_t>(
        std::sin(static_cast<double>(frame) * 0.071) * 12000.0);
    const auto right = static_cast<std::int16_t>(
        std::sin(static_cast<double>(frame) * 0.113) * 9000.0);
    result[frame * 2U] = left;
    result[frame * 2U + 1U] = right;
  }
  return result;
}

}  // namespace

int main() {
  using namespace telestudio::media;

  Require(!ValidateAacLd(
              {44100, 2, 192000, 23, AacLdTransport::Raw}),
          "AAC-LD must reject non-48-kHz PCM");
  Require(!ValidateAacLd(
              {48000, 2, 64000, 23, AacLdTransport::Raw}),
          "AAC-LD must reject unsupported stereo bitrates");

  const AacLdConfiguration configuration{
      48000, 2, 192000, 23, AacLdTransport::Raw};
  Require(ValidateAacLd(configuration),
          "AAC-LD stereo 192 kbps must be supported");

  AacLdEncoder encoder(configuration);
  const auto specificConfig = encoder.AudioSpecificConfig();
  Require(!specificConfig.empty(),
          "AAC-LD encoder must expose decoder configuration");
  AacLdDecoder decoder(configuration, specificConfig);

  const auto input = StereoFixture();
  const auto encoded = encoder.Encode(input);
  Require(!encoded.empty(), "AAC-LD must produce one access unit");
  const auto decoded = decoder.Decode(encoded);
  Require(decoded.size() == input.size(),
          "AAC-LD must decode one 512-sample stereo frame");
  Require(std::any_of(decoded.begin(), decoded.end(),
                      [](std::int16_t sample) { return sample != 0; }),
          "AAC-LD decoded PCM must contain signal");

  const AacLdConfiguration sipLatmConfiguration{
      48000, 2, 128000, 23, AacLdTransport::Latm};
  Mpeg4AacEncoder sipLatmEncoder(sipLatmConfiguration);
  const auto sipLatmInput = StereoFixture(512U);
  Require(!sipLatmEncoder.Encode(sipLatmInput).empty(),
          "SIP AAC-LD LATM must keep one native 512-sample frame");

  const AacLdConfiguration latmConfiguration{
      48000, 2, 128000, 23, AacLdTransport::Latm, 480, 2};
  Mpeg4AacEncoder latmEncoder(latmConfiguration);
  Mpeg4AacDecoder latmDecoder(latmConfiguration,
                              latmEncoder.AudioSpecificConfig());
  const auto latmInput = StereoFixture(480U);
  const auto firstLatmSubframe = latmEncoder.Encode(latmInput);
  Require(firstLatmSubframe.empty(),
          "AAC-LD LATM must wait for both 10-ms subframes");
  const auto latmEncoded = latmEncoder.Encode(latmInput);
  Require(!latmEncoded.empty(),
          "AAC-LD LATM must produce one 20-ms transport after two subframes");
  const auto latmDecoded = latmDecoder.Decode(latmEncoded);
  Require(latmDecoded.size() == latmInput.size() * 2U,
          "Cisco AAC-LD LATM must decode two 480-sample subframes");

  Require(latmEncoder.Encode(latmInput).empty(),
          "next LATM transport must also wait for its second subframe");
  const auto secondLatmEncoded = latmEncoder.Encode(latmInput);
  std::vector<std::uint8_t> twoFramePacket = latmEncoded;
  twoFramePacket.insert(twoFramePacket.end(), secondLatmEncoded.begin(),
                        secondLatmEncoded.end());
  const auto twoFrameDecoded = latmDecoder.Decode(twoFramePacket);
  Require(twoFrameDecoded.size() == latmInput.size() * 4U,
          "Cisco AAC-LD LATM must drain two consecutive 20-ms transports");

  for (const auto bitrate : {64000, 96000}) {
    Require(ValidateAacLd(
                {48000, 1, bitrate, 23, AacLdTransport::Raw}),
            "AAC-LD mono operating point must be supported");
  }
  for (const auto bitrate : {128000, 192000}) {
    Require(ValidateAacLd(
                {48000, 2, bitrate, 23, AacLdTransport::Raw}),
            "AAC-LD stereo operating point must be supported");
  }

  const AacLdConfiguration sony{48000, 2, 192000, 2, AacLdTransport::Raw};
  Require(ValidateMpeg4Aac(sony),
          "Sony MPEG-4 AAC-LC stereo 192 kbps must be supported");
  Mpeg4AacEncoder sonyEncoder(sony);
  Mpeg4AacDecoder sonyDecoder(sony, sonyEncoder.AudioSpecificConfig());
  std::vector<std::int16_t> sonyInput(1024U * 2U);
  for (std::size_t index = 0; index < sonyInput.size(); ++index)
    sonyInput[index] = static_cast<std::int16_t>((index % 101U) * 120 - 6000);
  const auto sonyEncoded = sonyEncoder.Encode(sonyInput);
  const auto sonyDecoded = sonyDecoder.Decode(sonyEncoded);
  Require(sonyDecoded.size() == sonyInput.size(),
          "Sony AAC-LC frame must round-trip at 48 kHz stereo");
  Mpeg4AacDecoder sonyDetailedDecoder(sony,
                                      sonyEncoder.AudioSpecificConfig());
  const auto sonyDetailed = sonyDetailedDecoder.DecodeDetailed(sonyEncoded);
  Require(sonyDetailed.sampleRate == 48000 && sonyDetailed.channels == 2 &&
              sonyDetailed.frameSize == 1024 &&
              sonyDetailed.pcm.size() == sonyInput.size(),
          "AAC decoder must expose authoritative 48-kHz stereo metadata");

  const AacLdConfiguration sonyReceive{
      32000, 1, 96000, 2, AacLdTransport::Raw};
  Require(ValidateMpeg4Aac(sonyReceive),
          "Sony MPEG-4 AAC-LC mono receive mode must support 32 kHz");
  Mpeg4AacEncoder sonyReceiveEncoder(sonyReceive);
  Mpeg4AacDecoder sonyReceiveDecoder(
      sonyReceive, sonyReceiveEncoder.AudioSpecificConfig());
  std::vector<std::int16_t> sonyReceiveInput(1024U, 900);
  const auto sonyReceiveEncoded = sonyReceiveEncoder.Encode(sonyReceiveInput);
  const auto sonyReceiveDecoded =
      sonyReceiveDecoder.DecodeDetailed(sonyReceiveEncoded);
  Require(sonyReceiveDecoded.sampleRate == 32000 &&
              sonyReceiveDecoded.channels == 1 &&
              sonyReceiveDecoded.frameSize == 1024 &&
              sonyReceiveDecoded.pcm.size() == 1024U,
          "Sony 32-kHz AAC-LC mono receive metadata must be exact");

  std::cout << "AAC-LD codec cases passed\n";
  return 0;
}
