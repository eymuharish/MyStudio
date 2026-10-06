#include <codec/opalplugin.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void Require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
int Control(const PluginCodec_Definition& def, void* context, const char* name, void* value, unsigned size) {
  for (auto* control = def.codecControls; control && control->name; ++control)
    if (!std::strcmp(control->name, name)) return control->control(&def, context, name, value, &size);
  throw std::runtime_error(std::string("Missing control: ") + name);
}
const PluginCodec_Option* Option(const PluginCodec_Definition& def, const char* name) {
  const PluginCodec_Option* const* options = nullptr;
  Require(Control(def, nullptr, PLUGINCODEC_CONTROL_GET_CODEC_OPTIONS, &options, sizeof(options)) == 1, "get_codec_options failed");
  for (; options && *options; ++options) if (!std::strcmp((*options)->m_name, name)) return *options;
  throw std::runtime_error(std::string("Missing option: ") + name);
}
bool Set(const PluginCodec_Definition& def, void* context, const char* name, const char* value) {
  const char* options[]{name, value, nullptr};
  return Control(def, context, PLUGINCODEC_CONTROL_SET_CODEC_OPTIONS, const_cast<char**>(options), sizeof(options[0])) == 1;
}
struct Instance {
  const PluginCodec_Definition& def;
  void* context;
  explicit Instance(const PluginCodec_Definition& value) : def(value), context(def.createCodec(&def)) { Require(context != nullptr, "AAC context creation failed"); }
  ~Instance() { def.destroyCodec(&def, context); }
};
unsigned Channels(const PluginCodec_Definition& def) { return ((def.flags & PluginCodec_ChannelsMask) >> PluginCodec_ChannelsPos) + 1; }
std::vector<std::int16_t> Fixture(unsigned samples, unsigned channels, unsigned block = 0) {
  std::vector<std::int16_t> pcm(samples * channels);
  for (unsigned i=0; i<samples; ++i) for (unsigned channel=0; channel<channels; ++channel)
    pcm[i*channels+channel] = static_cast<std::int16_t>(std::sin((i + block*samples) * (channel ? 0.113 : 0.071)) * 11000.0);
  return pcm;
}
std::vector<unsigned char> Encode(Instance& encoder, const std::vector<std::int16_t>& input) {
  std::vector<unsigned char> packet(8192);
  unsigned from = static_cast<unsigned>(input.size()*2), to = static_cast<unsigned>(packet.size()), flags = 0;
  Require(encoder.def.codecFunction(&encoder.def, encoder.context, input.data(), &from, packet.data(), &to, &flags) == 1, "AAC encode failed");
  Require(from == input.size()*2 && to != 0, "AAC did not consume one complete PCM block");
  packet.resize(to); return packet;
}
std::vector<std::int16_t> Decode(Instance& decoder, const std::vector<unsigned char>& input) {
  std::vector<std::int16_t> pcm(8192);
  unsigned from = static_cast<unsigned>(input.size()), to = static_cast<unsigned>(pcm.size()*2), flags = 0;
  Require(decoder.def.codecFunction(&decoder.def, decoder.context, input.data(), &from, pcm.data(), &to, &flags) == 1, "AAC decode failed");
  Require(from == input.size() && to != 0 && to % 2 == 0, "AAC decoded invalid PCM length");
  pcm.resize(to/2); return pcm;
}
void Roundtrip(const PluginCodec_Definition& enc, const PluginCodec_Definition& dec, const char* protocol) {
  Instance encoder(enc), decoder(dec);
  Require(Set(enc, encoder.context, "Protocol", protocol) && Set(dec, decoder.context, "Protocol", protocol), "Protocol option rejected");
  bool signal = false;
  for (unsigned i=0; i<12; ++i) {
    const auto input = Fixture(enc.parm.audio.samplesPerFrame, Channels(enc), i);
    const auto packet = Encode(encoder, input);
    const auto pcm = Decode(decoder, packet);
    Require(pcm.size() == input.size(), "Decoded rate/channels/frame length differs from negotiated PCM");
    signal = signal || std::any_of(pcm.begin(), pcm.end(), [](auto sample){ return std::abs(static_cast<int>(sample)) > 100; });
  }
  Require(signal, "Decoded AAC must contain a real signal");
  std::cout << enc.destFormat << " " << protocol << " PCM roundtrip passed\n";
}
void BoundaryChecks(const PluginCodec_Definition& enc, const PluginCodec_Definition& dec) {
  Instance encoder(enc), decoder(dec);
  Require(!Set(enc, encoder.context, "Target Bit Rate", "768000"), "Fixed AAC operating point accepted PCM target bitrate");
  auto input = Fixture(enc.parm.audio.samplesPerFrame, Channels(enc));
  std::vector<unsigned char> output(8192, 0x51);
  unsigned from = static_cast<unsigned>(input.size()*2 - 2), to = static_cast<unsigned>(output.size()), flags = 0;
  Require(enc.codecFunction(&enc, encoder.context, input.data(), &from, output.data(), &to, &flags) == 0, "Partial PCM frame accepted");
  Require(from == 0 && to == 0 && output[0] == 0x51, "Rejected PCM modified output or reported consumption");
  from = static_cast<unsigned>(input.size()*2); to = 1;
  Require(enc.codecFunction(&enc, encoder.context, input.data(), &from, output.data(), &to, &flags) == 0, "Too small encoded output accepted");
  Require(from == 0 && to == 0 && output[0] == 0x51, "Small output must be rejected before state advances");
  const auto packet = Encode(encoder, input);
  from = static_cast<unsigned>(packet.size()); to = 1;
  Require(dec.codecFunction(&dec, decoder.context, packet.data(), &from, output.data(), &to, &flags) == 0, "Too small PCM output accepted");
  const auto decoded = Decode(decoder, packet);
  Require(decoded.size() == input.size(), "Decoder failed after small buffer rejection");
  const std::vector<unsigned char> malformed{0xff, 0xff, 0xff};
  from = static_cast<unsigned>(malformed.size()); to = static_cast<unsigned>(output.size());
  const int malformedResult = dec.codecFunction(&dec, decoder.context, malformed.data(), &from, output.data(), &to, &flags);
  if (std::strstr(dec.sourceFormat, "AAC-LD")) {
    // LATM carries an AudioMuxElement directly. A transport sync error is a
    // dropped packet, not a fatal call error; it must never fabricate PCM.
    Require(malformedResult == 1 && from == malformed.size() && to == 0, "LATM sync error must consume the packet without PCM");
    Require(Decode(decoder, Encode(encoder, input)).size() == input.size(), "LATM failed to recover after an invalid transport packet");
  } else Require(malformedResult == 0, "Malformed RTP payload framing accepted");
  from = 100; to = static_cast<unsigned>(output.size());
  Require(dec.codecFunction(&dec, decoder.context, nullptr, &from, output.data(), &to, &flags) == 0, "Null input accepted");
}
void IndependentContexts(const PluginCodec_Definition& enc, const PluginCodec_Definition& dec) {
  Instance first(enc), second(enc);
  Instance firstDecoder(dec), secondDecoder(dec);
  const auto input = Fixture(enc.parm.audio.samplesPerFrame, Channels(enc));
  const auto one = Encode(first, input);
  const auto decodedOne = Decode(firstDecoder, one);
  Decode(firstDecoder, Encode(first, input));
  const auto two = Encode(second, input);
  Require(one == two, "Encoder state leaked between calls");
  Require(decodedOne == Decode(secondDecoder, two), "Decoder state leaked between calls");
}
}

int main(int argc, char** argv) {
  HMODULE library = nullptr;
  try {
    Require(argc == 2, "Pass the AAC plugin DLL path");
    library = LoadLibraryExA(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    Require(library != nullptr, "OPAL AAC plugin DLL is missing or cannot load");
    auto api = reinterpret_cast<PluginCodec_GetAPIVersionFunction>(GetProcAddress(library, PLUGIN_CODEC_API_VER_FN_STR));
    auto codecs = reinterpret_cast<PluginCodec_GetCodecFunction>(GetProcAddress(library, PLUGIN_CODEC_GET_CODEC_FN_STR));
    Require(api && codecs && api() == PWLIB_PLUGIN_API_VERSION, "Required OPAL plugin ABI exports missing");
    unsigned count = 123;
    Require(codecs(&count, PLUGIN_CODEC_VERSION - 1) == nullptr && count == 0, "Old plugin API must be rejected safely");
    const auto* definitions = codecs(&count, PLUGIN_CODEC_VERSION);
    Require(definitions && count == 12, "Expected six complete AAC encoder/decoder pairs");
    for (unsigned index=0; index<count; index+=2) {
      const auto& enc = definitions[index]; const auto& dec = definitions[index+1];
      Require(enc.version == PLUGIN_CODEC_VERSION && !std::strcmp(enc.sourceFormat, "L16") && !std::strcmp(dec.destFormat, "L16"), "Incorrect plugin definition or format pair");
      const auto* generic = static_cast<const PluginCodec_H323GenericCodecData*>(enc.h323CapabilityData);
      Require(enc.h323CapabilityType == PluginCodec_H323Codec_generic && generic && !std::strcmp(generic->standardIdentifier, "0.0.8.245.1.1.0"), "MPEG4 H.245 generic OID missing");
      Require((Option(enc, "AAC audio object type")->m_H245Generic & PluginCodec_H245_OrdinalMask) == 3, "AAC object H.245 ordinal incorrect");
      Require(((Option(enc, "AAC audio object type")->m_H245Generic & PluginCodec_H245_PositionMask) >> PluginCodec_H245_PositionShift) == 3, "AAC generic parameter ordering must follow canonical ordinal order");
      Require((Option(enc, "AAC profileAndLevel")->m_H245Generic & PluginCodec_H245_OrdinalMask) == 0, "AAC profile H.245 ordinal incorrect");
      Require(std::stoul(Option(enc, "AAC profileAndLevel")->m_minimum) == 0 && std::stoul(Option(enc, "AAC profileAndLevel")->m_maximum) >= 255, "H.245 must retain incompatible incoming profiles for equality validation instead of clamping");
      const bool ld = std::strstr(enc.destFormat, "AAC-LD") != nullptr;
      Require(!std::strcmp(Option(enc, "Clock Rate")->m_value, ld ? "90000" : enc.sampleRate == 32000 ? "32000" : "48000"), "OPAL Clock Rate must explicitly describe RTP transport clock");
      Require(!std::strcmp(Option(enc, "Frame Time")->m_value, ld ? "1800" : "1024"), "OPAL Frame Time must preserve exact AAC frame duration");
      Roundtrip(enc, dec, "H.323");
      char sip[] = "sip";
      const bool sipAllowed = Control(enc, nullptr, PLUGINCODEC_CONTROL_VALID_FOR_PROTOCOL, sip, sizeof(sip)) == 1;
      Require(sipAllowed == (enc.sampleRate == 48000), "Every 48k AAC mono/stereo operating point must support SIP; H323-only 32k stays separate");
      if (sipAllowed) { Roundtrip(enc, dec, "SIP"); BoundaryChecks(enc, dec); }
      else { Instance encoder(enc); Require(!Set(enc, encoder.context, "Protocol", "SIP"), "H.323-only codec accepted SIP"); BoundaryChecks(enc, dec); }
      IndependentContexts(enc, dec);
    }
    const auto& latm = definitions[0];
    Require(latm.sampleRate == 48000 && latm.parm.audio.samplesPerFrame == 960 && !std::strcmp(Option(latm, "RTP Clock Rate")->m_value, "90000"), "AAC-LD must keep PCM 48 kHz and RTP 90 kHz separate");
    Require(!std::strcmp(Option(latm, "AAC mux config present")->m_FMTPDefault, "1"), "MP4A-LATM omitted cpresent must default1 per RFC6416");
    {
      Instance fixed(latm);
      Require(Set(latm, fixed.context, "Clock Rate", "90000"), "LD transport clock options rejected");
      Require(Set(latm, fixed.context, "Frame Time", "1800"), "LD transport frame time rejected");
      Require(!Set(latm, fixed.context, "AAC profileAndLevel", "25"), "Cisco profile25 cannot use fixed64");
      Require(!Set(latm, fixed.context, "AAC audio object type", "2"), "Cisco object2 cannot use LD");
      Require(!Set(latm, fixed.context, "AAC bitrate", "128000"), "Cisco 128k cannot use fixed64");
      Require(!Set(latm, fixed.context, "AAC mux config present", "0"), "Cisco LATM needs cpresent=1");
      Require(!Set(latm, fixed.context, "RTP Clock Rate", "48000"), "Cisco RTP clock mutation accepted");
      Require(!Set(latm, fixed.context, "AAC profileAndLevel", "24junk"), "Malformed numeric options accepted");
      const auto& sony = definitions[4]; Instance lc(sony);
      Require(!Set(sony, lc.context, "AAC AudioSpecificConfig", "FFFF"), "Unsupported Sony AudioSpecificConfig accepted");
      Require(Set(sony, lc.context, "AAC AudioSpecificConfig", "1190"), "Valid Sony AudioSpecificConfig rejected");
    }
    FreeLibrary(library); library = nullptr;
    std::cout << "OPAL AAC exports, negotiated options, contexts and decoded PCM: all passed\n";
    return 0;
  } catch (const std::exception& error) {
    if (library) FreeLibrary(library);
    std::cerr << "FAIL: " << error.what() << '\n'; return 1;
  }
}
