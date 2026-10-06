#include <codec/opalplugin.h>
#include "telestudio/media/AacLdCodec.h"
#include "telestudio/media/AacLdPayload.h"
#include <aacdecoder_lib.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {
using namespace telestudio::media;
constexpr unsigned kMaximumPacket = 4096;
constexpr int kAllH245 = PluginCodec_H245_TCS | PluginCodec_H245_OLC | PluginCodec_H245_ReqMode;
constexpr int kNonCollapsing = PluginCodec_H245_NonCollapsing | kAllH245;
constexpr int kCollapsing = PluginCodec_H245_Collapsing | kAllH245;
constexpr const char* kMpeg4Oid = "0.0.8.245.1.1.0";

bool EqualNoCase(std::string_view one, std::string_view two) {
  return one.size() == two.size() && std::equal(one.begin(), one.end(), two.begin(), [](auto a, auto b) {
    return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
  });
}
bool Numeric(std::string_view value, unsigned expected) {
  unsigned parsed = 0;
  const auto result = std::from_chars(value.data(), value.data()+value.size(), parsed);
  return !value.empty() && result.ec == std::errc{} && result.ptr == value.data()+value.size() && parsed == expected;
}
std::string Hex(std::span<const std::uint8_t> bytes) {
  constexpr char digits[] = "0123456789ABCDEF";
  std::string result;
  for (auto byte : bytes) { result += digits[byte >> 4]; result += digits[byte & 15]; }
  return result;
}
std::string CompactHex(std::string_view text) {
  std::string result;
  for (auto ch : text) {
    if (std::isspace(static_cast<unsigned char>(ch))) continue;
    if (!std::isxdigit(static_cast<unsigned char>(ch))) return {};
    result += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  }
  return result;
}
struct Format {
  const char* name = nullptr;
  AacLdConfiguration configuration;
  unsigned samples = 0;
  unsigned profile = 0;
  bool sip = false;
  std::vector<std::uint8_t> audioSpecificConfig;
  std::deque<std::string> values;
  std::vector<PluginCodec_Option> options;
  std::vector<const PluginCodec_Option*> optionPointers;
  PluginCodec_H323GenericCodecData generic{};

  void Add(PluginCodec_OptionTypes type, const char* name, std::string value, const char* fmtp = nullptr,
           int h245 = 0, PluginCodec_OptionMerge merge = PluginCodec_EqualMerge) {
    values.push_back(std::move(value));
    const auto* text = values.back().c_str();
    PluginCodec_Option option{};
    option.m_type = type; option.m_name = name; option.m_readOnly = 1; option.m_merge = merge;
    // SDP applies every nonempty FMTP default, even to options with no FMTP
    // name. H.245-only options and frame/clock metadata must stay untouched.
    option.m_value = text; option.m_FMTPName = fmtp; option.m_FMTPDefault = fmtp ? "0" : nullptr;
    // Cisco's generic parameter sequence follows ordinal order. OPAL sorts
    // options by the separately encoded H.245 position before creating PDUs.
    option.m_H245Generic = h245 | ((h245 & PluginCodec_H245_OrdinalMask) << PluginCodec_H245_PositionShift);
    // OPAL's H.245 reader calls SetValue(), which clamps numeric options.
    // Keep peer values representable so equality negotiation rejects an
    // incompatible profile/object instead of silently turning it into ours.
    if (type == PluginCodec_IntegerOption) { option.m_minimum = "0"; option.m_maximum = "2147483647"; }
    if (type == PluginCodec_EnumOption) option.m_minimum = text;
    options.push_back(option);
  }
  void Initialise(const char* formatName, int channels, int rate, bool ld, bool forSip) {
    name = formatName; sip = forSip;
    configuration = {rate, channels, ld ? (channels == 1 ? 64000 : 128000) : (channels == 1 ? 96000 : 192000),
                     ld ? 23 : 2, ld ? AacLdTransport::Latm : AacLdTransport::Raw, ld ? 480 : 0, ld ? 2 : 1};
    samples = ld ? 960 : 1024; profile = ld ? (channels == 1 ? 24 : 25) : 15;
    Mpeg4AacEncoder probe(configuration);
    audioSpecificConfig = probe.AudioSpecificConfig();
    generic = {kMpeg4Oid, static_cast<unsigned>(configuration.bitrate), 0, nullptr};
    options.reserve(22);
    Add(PluginCodec_IntegerOption, "AAC profileAndLevel", std::to_string(profile), "profile-level-id", 0 | kNonCollapsing);
    Add(PluginCodec_IntegerOption, "AAC audio object type", std::to_string(configuration.audioObjectType), ld ? "object" : nullptr, 3 | kNonCollapsing);
    Add(PluginCodec_IntegerOption, "AAC bitrate", std::to_string(configuration.bitrate), ld ? "bitrate" : nullptr);
    // The pinned OPAL core does not use its separate RTP Clock Rate option.
    // Override transport clock and exact frame ticks through its normal media
    // options; definition.sampleRate still selects the correct raw PCM format.
    // Codec selection merges PCM and encoded formats in both directions.
    // Each side retains its own clock/frame units; SDP rtpmap validates the
    // peer transport clock, and SetOptions still enforces the encoded values.
    Add(PluginCodec_IntegerOption, PLUGINCODEC_OPTION_CLOCK_RATE, ld ? "90000" : std::to_string(rate), nullptr, 0, PluginCodec_NoMerge);
    Add(PluginCodec_IntegerOption, PLUGINCODEC_OPTION_FRAME_TIME, ld ? "1800" : "1024", nullptr, 0, PluginCodec_NoMerge);
    // PCM's uncompressed target rate must not replace this fixed AAC
    // operating point when OPAL selects the decoder via the raw format.
    Add(PluginCodec_IntegerOption, PLUGINCODEC_OPTION_TARGET_BIT_RATE, std::to_string(configuration.bitrate), nullptr, 0, PluginCodec_NoMerge);
    Add(PluginCodec_IntegerOption, PLUGINCODEC_OPTION_RTP_CLOCK_RATE, ld ? "90000" : std::to_string(rate));
    Add(PluginCodec_IntegerOption, PLUGINCODEC_OPTION_RTP_CHANNELS, std::to_string(channels));
    Add(PluginCodec_IntegerOption, "AAC max AL-SDU frames", ld ? "2" : "1", nullptr, 2 | kCollapsing, PluginCodec_MinMerge);
    if (ld) {
      Add(PluginCodec_BoolOption, "AAC LATM", "1", nullptr, 1 | kNonCollapsing);
      Add(PluginCodec_IntegerOption, "AAC max audio objects", "1", nullptr, 5 | kCollapsing, PluginCodec_MinMerge);
      Add(PluginCodec_BoolOption, "AAC mux config present", "1", "cpresent", 6 | PluginCodec_H245_NonCollapsing | PluginCodec_H245_OLC);
      // RFC6416 section7.3: omitted cpresent defaults to in-band config (1).
      // Cisco SX20 omits it in a valid profile24/object23/64k SDP answer.
      options.back().m_FMTPDefault = "1";
    } else {
      Add(PluginCodec_OctetsOption, "AAC AudioSpecificConfig", Hex(audioSpecificConfig), "config", 4 | PluginCodec_H245_NonCollapsing | PluginCodec_H245_OLC | PluginCodec_H245_ReqMode);
      Add(PluginCodec_IntegerOption, "AAC streamtype", "5", "streamtype");
      // OPAL appends parsed FMTP strings to the current string option value;
      // an enum represents this single supported mode without concatenation.
      Add(PluginCodec_EnumOption, "AAC mode", "AAC-hbr", "mode");
      Add(PluginCodec_IntegerOption, "AAC sizeLength", "13", "sizelength");
      Add(PluginCodec_IntegerOption, "AAC indexLength", "3", "indexlength");
      Add(PluginCodec_IntegerOption, "AAC indexDeltaLength", "3", "indexdeltalength");
    }
    for (const auto& option : options) optionPointers.push_back(&option);
    optionPointers.push_back(nullptr);
  }
};

// A LATM receiver can join between in-band StreamMuxConfig repetitions, or
// reopen after hold while the remote encoder continues. FDK explicitly treats
// missing bits and transport synchronisation as recoverable: keep the context
// and consume that packet without PCM rather than terminating OPAL's patch.
class LatmDecoder {
  HANDLE_AACDECODER handle = nullptr;
  unsigned diagnostics = 0;
public:
  LatmDecoder() : handle(aacDecoder_Open(TT_MP4_LATM_MCP1, 1)) {
    if (!handle) throw std::runtime_error("LATM decoder allocation failed");
  }
  ~LatmDecoder() { aacDecoder_Close(handle); }
  DecodedAacAudio Decode(std::span<const std::uint8_t> unit) {
    auto* input = const_cast<UCHAR*>(unit.data());
    UINT size = static_cast<UINT>(unit.size()), remaining = size;
    if (aacDecoder_Fill(handle, &input, &size, &remaining) != AAC_DEC_OK || remaining != 0)
      throw std::runtime_error("LATM decoder input buffer failed");
    DecodedAacAudio audio;
    for (unsigned frame=0; frame<8; ++frame) {
      std::array<INT_PCM, 4096> pcm{};
      const auto error = aacDecoder_DecodeFrame(handle, pcm.data(), static_cast<INT>(pcm.size()), 0);
      if (error == AAC_DEC_NOT_ENOUGH_BITS || error == AAC_DEC_TRANSPORT_SYNC_ERROR) {
        if (audio.pcm.empty() && diagnostics < 8) {
          const auto* enabled = std::getenv("ESSASTUDIO_DIAGNOSTIC_LOG");
          if (enabled && std::strcmp(enabled, "1") == 0) {
            std::fprintf(stderr, "[OPAL AAC] LATM waiting for mux configuration: FDK=0x%04x bytes=%zu useSameStreamMux=%u\n",
                         static_cast<unsigned>(error), unit.size(), static_cast<unsigned>(unit.front() >> 7));
            ++diagnostics;
          }
        }
        break;
      }
      if (error != AAC_DEC_OK) throw std::runtime_error("LATM decoder rejected audio frame: FDK=" + std::to_string(error));
      const auto* info = aacDecoder_GetStreamInfo(handle);
      if (!info || info->sampleRate != 48000 || info->aot != AOT_ER_AAC_LD ||
          info->frameSize != 480 || info->numChannels < 1 || info->numChannels > 2)
        throw std::runtime_error("LATM decoder received unsupported stream configuration: rate=" + std::to_string(info ? info->sampleRate : 0) +
                                 " object=" + std::to_string(info ? info->aot : AOT_NONE) +
                                 " frame=" + std::to_string(info ? info->frameSize : 0) + " channels=" + std::to_string(info ? info->numChannels : 0));
      if (audio.sampleRate && (audio.sampleRate != info->sampleRate || audio.channels != info->numChannels))
        throw std::runtime_error("LATM layout changed within one packet");
      audio.sampleRate = info->sampleRate; audio.channels = info->numChannels; audio.frameSize = info->frameSize;
      audio.pcm.insert(audio.pcm.end(), pcm.begin(), pcm.begin() + info->frameSize*info->numChannels);
    }
    return audio;
  }
};

struct Context {
  const Format& format;
  bool encoder;
  bool sip;
  unsigned diagnostics = 0;
  std::mutex mutex;
  std::unique_ptr<Mpeg4AacEncoder> encoding;
  std::unique_ptr<Mpeg4AacDecoder> decoding;
  std::unique_ptr<LatmDecoder> latmDecoding;
  explicit Context(const Format& value, bool encode) : format(value), encoder(encode), sip(value.sip) { Reset(); }
  void Reset() {
    if (encoder) encoding = std::make_unique<Mpeg4AacEncoder>(format.configuration);
    else if (format.configuration.transport == AacLdTransport::Latm) latmDecoding = std::make_unique<LatmDecoder>();
    else decoding = std::make_unique<Mpeg4AacDecoder>(format.configuration, format.audioSpecificConfig);
  }
};
const Format* GetFormat(const PluginCodec_Definition* def) { return def ? static_cast<const Format*>(def->userData) : nullptr; }
void* Create(const PluginCodec_Definition* def) {
  try { const auto* fmt = GetFormat(def); return fmt ? new Context(*fmt, std::strcmp(def->sourceFormat, "L16") == 0) : nullptr; }
  catch (...) { return nullptr; }
}
void Destroy(const PluginCodec_Definition*, void* context) { delete static_cast<Context*>(context); }
int GetOptions(const PluginCodec_Definition* def, void*, const char*, void* parm, unsigned* size) {
  const auto* fmt = GetFormat(def);
  if (!fmt || !parm || !size || *size != sizeof(const PluginCodec_Option* const*)) return 0;
  *static_cast<const PluginCodec_Option* const**>(parm) = fmt->optionPointers.data(); *size = 0; return 1;
}
int ValidProtocol(const PluginCodec_Definition* def, void*, const char*, void* parm, unsigned*) {
  const auto* fmt = GetFormat(def); if (!fmt || !parm) return 0;
  const std::string_view protocol(static_cast<const char*>(parm));
  return EqualNoCase(protocol, "H.323") || EqualNoCase(protocol, "H323") || (fmt->sip && EqualNoCase(protocol, "SIP"));
}
int SetOptions(const PluginCodec_Definition* def, void* opaque, const char*, void* parm, unsigned* size) {
  const auto* fmt = GetFormat(def); auto* context = static_cast<Context*>(opaque);
  if (!fmt || !context || &context->format != fmt || !parm || !size || *size != sizeof(const char**)) return 0;
  std::lock_guard lock(context->mutex);
  try {
    auto** options = static_cast<const char**>(parm);
    bool sip = context->sip;
    std::unordered_set<std::string> seen;
    for (unsigned index=0; options[index]; index+=2) {
      if (index > 128 || !options[index+1]) return 0;
      const std::string_view name(options[index]), value(options[index+1]);
      if (name == PLUGINCODEC_OPTION_PROTOCOL) {
        if (!seen.insert(std::string(name)).second) return 0;
        if (EqualNoCase(value, "SIP")) { if (!fmt->sip) return 0; sip = true; }
        else if (EqualNoCase(value, "H.323") || EqualNoCase(value, "H323")) sip = false;
        else return 0;
        continue;
      }
      if (name == PLUGINCODEC_OPTION_CHANNELS) {
        if (!Numeric(value, fmt->configuration.channels)) return 0;
        continue;
      }
      for (const auto& option : fmt->options) if (name == option.m_name) {
        if (!seen.insert(std::string(name)).second) return 0;
        if (option.m_type == PluginCodec_IntegerOption) {
          unsigned expected = 0; const auto expectedText = std::string_view(option.m_value);
          std::from_chars(expectedText.data(), expectedText.data()+expectedText.size(), expected);
          if (!Numeric(value, expected)) return 0;
        } else if (option.m_type == PluginCodec_BoolOption) {
          if (value != "1" && !EqualNoCase(value, "true")) return 0;
        } else if (option.m_type == PluginCodec_OctetsOption) {
          if (CompactHex(value) != option.m_value) return 0;
        } else if (!EqualNoCase(value, option.m_value)) return 0;
      }
    }
    context->sip = sip;
    return 1;
  } catch (...) { return 0; }
}
int OutputSize(const PluginCodec_Definition* def, void*, const char*, void*, unsigned*) {
  const auto* fmt = GetFormat(def); if (!fmt) return 0;
  return std::strcmp(def->sourceFormat, "L16") == 0 ? kMaximumPacket : fmt->samples * fmt->configuration.channels * sizeof(std::int16_t);
}
int Transcode(const PluginCodec_Definition* def, void* opaque, const void* from, unsigned* fromLength,
              void* to, unsigned* toLength, unsigned* flags) {
  auto* context = static_cast<Context*>(opaque);
  const unsigned availableInput = fromLength ? *fromLength : 0;
  const unsigned availableOutput = toLength ? *toLength : 0;
  if (fromLength) *fromLength = 0;
  if (toLength) *toLength = 0;
  if (flags) *flags = 0;
  if (!context || GetFormat(def) != &context->format || !from || !to || !fromLength || !toLength || !flags) return 0;
  std::lock_guard lock(context->mutex);
  const auto& fmt = context->format;
  const unsigned pcmBytes = fmt.samples * fmt.configuration.channels * sizeof(std::int16_t);
  auto reject = [&](const char* reason) {
    const auto* enabled = std::getenv("ESSASTUDIO_DIAGNOSTIC_LOG");
    if (enabled && std::strcmp(enabled, "1") == 0 && context->diagnostics++ < 8)
      std::fprintf(stderr, "[OPAL AAC] %s %s rejected: input=%u outputCapacity=%u expectedPCM=%u protocol=%s reason=%s\n",
                   fmt.name, context->encoder ? "encode" : "decode", availableInput, availableOutput, pcmBytes,
                   context->sip ? "SIP" : "H.323", reason);
    return 0;
  };
  try {
    if (context->encoder) {
      // Validate capacity before touching the stateful FDK encoder.
      if (availableInput != pcmBytes || availableOutput < kMaximumPacket) return reject("PCM input or encoded output capacity");
      std::vector<std::int16_t> pcm(pcmBytes/2);
      std::memcpy(pcm.data(), from, pcmBytes);
      std::vector<std::uint8_t> encoded;
      if (fmt.configuration.transport == AacLdTransport::Latm) {
        const auto subframe = static_cast<std::size_t>(480 * fmt.configuration.channels);
        for (std::size_t offset=0; offset<pcm.size(); offset+=subframe) {
          auto transport = context->encoding->Encode(std::span(pcm).subspan(offset, subframe));
          if (!transport.empty()) encoded = std::move(transport);
        }
      } else encoded = context->encoding->Encode(pcm);
      if (encoded.empty()) return 0;
      // RFC6416 section6.1: an AudioMuxElement starts at the first RTP
      // payload byte. FDK LATM already is that element, without a length prefix.
      if (context->sip && fmt.configuration.transport == AacLdTransport::Raw) encoded = PackRfc3640(encoded);
      if (encoded.size() > availableOutput) return 0;
      std::memcpy(to, encoded.data(), encoded.size());
      *fromLength = availableInput; *toLength = static_cast<unsigned>(encoded.size()); return 1;
    }
    if (availableInput == 0 || availableInput > kMaximumPacket || availableOutput < pcmBytes) return reject("encoded input or PCM output capacity");
    const auto packet = std::span(static_cast<const std::uint8_t*>(from), availableInput);
    std::vector<std::uint8_t> accessUnit;
    if (context->sip && fmt.configuration.transport == AacLdTransport::Raw) accessUnit = UnpackRfc3640(packet);
    else accessUnit.assign(packet.begin(), packet.end());
    if (accessUnit.empty()) return reject("RTP access-unit framing");
    const auto audio = context->latmDecoding ? context->latmDecoding->Decode(accessUnit) : context->decoding->DecodeDetailed(accessUnit);
    if (context->latmDecoding && audio.pcm.empty()) {
      *fromLength = availableInput;
      return 1;
    }
    if (audio.sampleRate != fmt.configuration.sampleRate || audio.channels < 1 || audio.channels > 2 ||
        audio.pcm.size() != static_cast<std::size_t>(fmt.samples * audio.channels)) return reject("decoded PCM rate, channels or length");
    std::vector<std::int16_t> pcm;
    if (audio.channels == fmt.configuration.channels) pcm = audio.pcm;
    else {
      // Cisco LATM stream configuration is authoritative even when H.245 says stereo.
      pcm.resize(pcmBytes/2);
      for (unsigned index=0; index<fmt.samples; ++index) {
        if (fmt.configuration.channels == 1)
          pcm[index] = static_cast<std::int16_t>((static_cast<int>(audio.pcm[index*2]) + audio.pcm[index*2+1])/2);
        else pcm[index*2] = pcm[index*2+1] = audio.pcm[index];
      }
    }
    std::memcpy(to, pcm.data(), pcmBytes);
    *fromLength = availableInput; *toLength = pcmBytes; return 1;
  } catch (const std::exception& error) {
    const auto* enabled = std::getenv("ESSASTUDIO_DIAGNOSTIC_LOG");
    if (enabled && std::strcmp(enabled, "1") == 0 && context->diagnostics++ < 8)
      std::fprintf(stderr, "[OPAL AAC] %s %s failed: bytes=%u %s\n", fmt.name, context->encoder ? "encode" : "decode", availableInput, error.what());
    try { context->Reset(); } catch (...) {}
    return 0;
  } catch (...) {
    try { context->Reset(); } catch (...) {}
    return 0;
  }
}
PluginCodec_ControlDefn controls[]{
  {PLUGINCODEC_CONTROL_GET_CODEC_OPTIONS, GetOptions},
  {PLUGINCODEC_CONTROL_VALID_FOR_PROTOCOL, ValidProtocol},
  {PLUGINCODEC_CONTROL_SET_CODEC_OPTIONS, SetOptions},
  {PLUGINCODEC_CONTROL_GET_OUTPUT_DATA_SIZE, OutputSize},
  {nullptr, nullptr}
};
const PluginCodec_information information{
  0, "MyStudio", "1.0", nullptr, nullptr, "MyStudio", "Project license", PluginCodec_License_None,
  "FDK-AAC AAC-LD and AAC-LC", "Fraunhofer", "2.0.3", nullptr, "https://github.com/mstorsjo/fdk-aac",
  "Fraunhofer", "Fraunhofer FDK AAC Codec Library for Android license", PluginCodec_License_RoyaltiesRequired, __DATE__
};
struct Registry {
  std::array<Format, 6> formats;
  std::array<PluginCodec_Definition, 12> definitions{};
  Registry() {
    formats[0].Initialise("MyStudio-AAC-LD-LATM-64", 1, 48000, true, true);
    formats[1].Initialise("MyStudio-AAC-LD-LATM-128", 2, 48000, true, true);
    formats[2].Initialise("MyStudio-AAC-LC-48k-stereo192", 2, 48000, false, true);
    formats[3].Initialise("MyStudio-AAC-LC-48k-mono96", 1, 48000, false, true);
    formats[4].Initialise("MyStudio-AAC-LC-32k-stereo192", 2, 32000, false, false);
    formats[5].Initialise("MyStudio-AAC-LC-32k-mono96", 1, 32000, false, false);
    for (std::size_t i=0; i<formats.size(); ++i) for (unsigned direction=0; direction<2; ++direction) {
      auto& fmt = formats[i]; auto& def = definitions[i*2+direction];
      def.version = PLUGIN_CODEC_VERSION; def.info = &information;
      def.flags = PluginCodec_MediaTypeAudio | PluginCodec_RTPTypeDynamic | PluginCodec_SetChannels(fmt.configuration.channels);
      def.descr = fmt.name; def.sourceFormat = direction ? fmt.name : "L16"; def.destFormat = direction ? "L16" : fmt.name;
      def.userData = &fmt; def.sampleRate = fmt.configuration.sampleRate; def.bitsPerSec = fmt.configuration.bitrate;
      def.usPerFrame = static_cast<unsigned>(static_cast<std::uint64_t>(fmt.samples)*1000000 / fmt.configuration.sampleRate);
      def.parm.audio = {fmt.samples, kMaximumPacket, 1, 1}; def.rtpPayload = fmt.configuration.audioObjectType == 23 ? 108 : 96;
      def.sdpFormat = fmt.configuration.audioObjectType == 23 ? "MP4A-LATM" : "MPEG4-GENERIC";
      def.createCodec = Create; def.destroyCodec = Destroy; def.codecFunction = Transcode; def.codecControls = controls;
      def.h323CapabilityType = PluginCodec_H323Codec_generic; def.h323CapabilityData = &fmt.generic;
    }
  }
};
}

extern "C" {
PLUGIN_CODEC_API_VER_FN_DECLARE
PLUGIN_CODEC_DLL_API const PluginCodec_Definition* OpalCodecPlugin_GetCodecs(unsigned* count, unsigned version) {
  if (!count) return nullptr;
  *count = 0;
  if (version < PLUGIN_CODEC_VERSION) return nullptr;
  try { static Registry registry; *count = static_cast<unsigned>(registry.definitions.size()); return registry.definitions.data(); }
  catch (...) { return nullptr; }
}
}
