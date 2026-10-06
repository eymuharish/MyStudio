#include "telestudio/media/AacLdCodec.h"

#include <aacdecoder_lib.h>
#include <aacenc_lib.h>

#include <array>
#include <stdexcept>
#include <string>
#include <utility>

namespace telestudio::media {
namespace {

[[noreturn]] void ThrowCodecError(const char* operation, int error) {
  throw std::runtime_error(std::string(operation) + " failed (" +
                           std::to_string(error) + ")");
}

CHANNEL_MODE ChannelMode(int channels) {
  return channels == 1 ? MODE_1 : MODE_2;
}

TRANSPORT_TYPE Transport(AacLdTransport transport) {
  return transport == AacLdTransport::Raw ? TT_MP4_RAW : TT_MP4_LATM_MCP1;
}

}  // namespace

bool ValidateAacLd(const AacLdConfiguration& configuration) noexcept {
  return configuration.sampleRate == 48000 &&
         configuration.audioObjectType == 23 &&
         ValidateMpeg4Aac(configuration);
}

bool ValidateMpeg4Aac(const AacLdConfiguration& configuration) noexcept {
  if ((configuration.sampleRate != 32000 &&
       configuration.sampleRate != 48000) ||
      (configuration.audioObjectType != 2 && configuration.audioObjectType != 23))
    return false;
  if (configuration.channels == 1)
    return configuration.bitrate == 64000 || configuration.bitrate == 96000;
  if (configuration.channels == 2)
    return configuration.bitrate == 128000 || configuration.bitrate == 192000;
  return false;
}

struct Mpeg4AacEncoder::Impl {
  AacLdConfiguration configuration;
  HANDLE_AACENCODER handle{nullptr};
  AACENC_InfoStruct info{};
  std::vector<std::uint8_t> specificConfig;

  explicit Impl(AacLdConfiguration value) : configuration(value) {
    if (!ValidateMpeg4Aac(configuration))
      throw std::invalid_argument("Unsupported MPEG-4 AAC operating point");
    auto error = aacEncOpen(&handle, 0, configuration.channels);
    if (error != AACENC_OK) ThrowCodecError("aacEncOpen", error);
    try {
      const auto set = [this](AACENC_PARAM parameter, UINT value) {
        const auto result = aacEncoder_SetParam(handle, parameter, value);
        if (result != AACENC_OK) ThrowCodecError("aacEncoder_SetParam", result);
      };
      set(AACENC_AOT, configuration.audioObjectType);
      set(AACENC_SAMPLERATE, configuration.sampleRate);
      set(AACENC_CHANNELMODE, ChannelMode(configuration.channels));
      set(AACENC_CHANNELORDER, 1);
      set(AACENC_BITRATE, configuration.bitrate);
      set(AACENC_TRANSMUX, Transport(configuration.transport));
      // H.323 and SIP negotiate different AAC-LD packetization. Keep the
      // native FDK frame unless the protocol adapter explicitly requests a
      // granule/subframe layout.
      if (configuration.granuleLength > 0) {
        set(AACENC_GRANULE_LENGTH,
            static_cast<UINT>(configuration.granuleLength));
      }
      if (configuration.transportSubframes > 1) {
        set(AACENC_TPSUBFRAMES,
            static_cast<UINT>(configuration.transportSubframes));
      }
      set(AACENC_AFTERBURNER, 1);
      error = aacEncEncode(handle, nullptr, nullptr, nullptr, nullptr);
      if (error != AACENC_OK) ThrowCodecError("aacEncEncode initialization", error);
      error = aacEncInfo(handle, &info);
      if (error != AACENC_OK) ThrowCodecError("aacEncInfo", error);
      specificConfig.assign(info.confBuf, info.confBuf + info.confSize);
    } catch (...) {
      aacEncClose(&handle);
      throw;
    }
  }

  ~Impl() { aacEncClose(&handle); }
};

Mpeg4AacEncoder::Mpeg4AacEncoder(AacLdConfiguration configuration)
    : impl_(std::make_unique<Impl>(configuration)) {}
Mpeg4AacEncoder::~Mpeg4AacEncoder() = default;
Mpeg4AacEncoder::Mpeg4AacEncoder(Mpeg4AacEncoder&&) noexcept = default;
Mpeg4AacEncoder& Mpeg4AacEncoder::operator=(Mpeg4AacEncoder&&) noexcept = default;

const AacLdConfiguration& Mpeg4AacEncoder::Configuration() const noexcept {
  return impl_->configuration;
}

const std::vector<std::uint8_t>& Mpeg4AacEncoder::AudioSpecificConfig() const noexcept {
  return impl_->specificConfig;
}

std::vector<std::uint8_t> Mpeg4AacEncoder::Encode(
    std::span<const std::int16_t> pcm) {
  const auto expected = static_cast<std::size_t>(impl_->info.frameLength) *
                        static_cast<std::size_t>(impl_->configuration.channels);
  if (pcm.size() != expected)
    throw std::invalid_argument("AAC-LD PCM block must contain one complete frame");

  std::array<std::uint8_t, 32768> output{};
  void* inputPointer = const_cast<std::int16_t*>(pcm.data());
  void* outputPointer = output.data();
  int inputIdentifier = IN_AUDIO_DATA;
  int outputIdentifier = OUT_BITSTREAM_DATA;
  int inputSize = static_cast<int>(pcm.size_bytes());
  int outputSize = static_cast<int>(output.size());
  int inputElementSize = sizeof(std::int16_t);
  int outputElementSize = sizeof(std::uint8_t);
  AACENC_BufDesc inputDescriptor{1, &inputPointer, &inputIdentifier, &inputSize,
                                 &inputElementSize};
  AACENC_BufDesc outputDescriptor{1, &outputPointer, &outputIdentifier,
                                  &outputSize, &outputElementSize};
  AACENC_InArgs inputArguments{};
  inputArguments.numInSamples = static_cast<int>(pcm.size());
  AACENC_OutArgs outputArguments{};
  const auto error = aacEncEncode(impl_->handle, &inputDescriptor,
                                  &outputDescriptor, &inputArguments,
                                  &outputArguments);
  if (error != AACENC_OK) ThrowCodecError("aacEncEncode", error);
  return {output.begin(), output.begin() + outputArguments.numOutBytes};
}

struct Mpeg4AacDecoder::Impl {
  AacLdConfiguration configuration;
  HANDLE_AACDECODER handle{nullptr};

  Impl(AacLdConfiguration value,
       std::span<const std::uint8_t> audioSpecificConfig)
      : configuration(value), handle(aacDecoder_Open(Transport(value.transport), 1)) {
    if (!ValidateMpeg4Aac(configuration))
      throw std::invalid_argument("Unsupported MPEG-4 AAC operating point");
    if (handle == nullptr) throw std::runtime_error("aacDecoder_Open failed");
    if (configuration.transport == AacLdTransport::Raw) {
      if (audioSpecificConfig.empty())
        throw std::invalid_argument("AAC-LD raw transport requires AudioSpecificConfig");
      auto* config = const_cast<UCHAR*>(audioSpecificConfig.data());
      UINT configSize = static_cast<UINT>(audioSpecificConfig.size());
      const auto error = aacDecoder_ConfigRaw(handle, &config, &configSize);
      if (error != AAC_DEC_OK) {
        aacDecoder_Close(handle);
        handle = nullptr;
        ThrowCodecError("aacDecoder_ConfigRaw", error);
      }
    }
  }

  ~Impl() { aacDecoder_Close(handle); }
};

Mpeg4AacDecoder::Mpeg4AacDecoder(
    AacLdConfiguration configuration,
    std::span<const std::uint8_t> audioSpecificConfig)
    : impl_(std::make_unique<Impl>(configuration, audioSpecificConfig)) {}
Mpeg4AacDecoder::~Mpeg4AacDecoder() = default;
Mpeg4AacDecoder::Mpeg4AacDecoder(Mpeg4AacDecoder&&) noexcept = default;
Mpeg4AacDecoder& Mpeg4AacDecoder::operator=(Mpeg4AacDecoder&&) noexcept = default;

std::vector<std::int16_t> Mpeg4AacDecoder::Decode(
    std::span<const std::uint8_t> accessUnit) {
  return DecodeDetailed(accessUnit).pcm;
}

DecodedAacAudio Mpeg4AacDecoder::DecodeDetailed(
    std::span<const std::uint8_t> accessUnit) {
  if (accessUnit.empty()) return {};
  auto* input = const_cast<UCHAR*>(accessUnit.data());
  UINT inputSize = static_cast<UINT>(accessUnit.size());
  UINT bytesValid = inputSize;
  auto error = aacDecoder_Fill(impl_->handle, &input, &inputSize, &bytesValid);
  if (error != AAC_DEC_OK) ThrowCodecError("aacDecoder_Fill", error);

  DecodedAacAudio output;
  for (unsigned frame = 0; frame < 8; ++frame) {
    // FDK reports the authoritative stream layout after decoding. Allocate
    // for the supported maximum instead of sizing from the local guess.
    std::vector<std::int16_t> decoded(4096U);
    error = aacDecoder_DecodeFrame(impl_->handle, decoded.data(),
                                   static_cast<INT>(decoded.size()), 0);
    if (error == AAC_DEC_NOT_ENOUGH_BITS) break;
    if (error != AAC_DEC_OK) ThrowCodecError("aacDecoder_DecodeFrame", error);
    const auto* info = aacDecoder_GetStreamInfo(impl_->handle);
    if (info == nullptr || info->frameSize <= 0 || info->numChannels <= 0)
      throw std::runtime_error(
          "AAC-LD decoder returned invalid stream information");
    if (info->sampleRate <= 0 || info->numChannels > 2) {
      throw std::runtime_error(
          "AAC-LD decoder returned unsupported stream information");
    }
    if (output.sampleRate != 0 &&
        (output.sampleRate != info->sampleRate ||
         output.channels != info->numChannels ||
         output.frameSize != info->frameSize)) {
      throw std::runtime_error("AAC stream layout changed inside one payload");
    }
    output.sampleRate = info->sampleRate;
    output.channels = info->numChannels;
    output.frameSize = info->frameSize;
    decoded.resize(static_cast<std::size_t>(info->frameSize) *
                   static_cast<std::size_t>(info->numChannels));
    output.pcm.insert(output.pcm.end(), decoded.begin(), decoded.end());
  }
  if (output.pcm.empty()) ThrowCodecError("aacDecoder_DecodeFrame", error);
  return output;
}

}  // namespace telestudio::media
