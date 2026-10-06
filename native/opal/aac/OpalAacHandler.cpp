#include "OpalAacHandler.h"
#include <ptlib.h>
#include <codec/opalpluginmgr.h>
#include <asn/h245.h>
#include <memory>
#include <cstring>

namespace {
class AacFramedTranscoder : public OpalPluginFramedAudioTranscoder {
  PCLASSINFO(AacFramedTranscoder, OpalPluginFramedAudioTranscoder);
public:
  AacFramedTranscoder(const OpalTranscoderKey& key, const PluginCodec_Definition* codec, bool encoder)
    : OpalPluginFramedAudioTranscoder(key, codec, encoder) {}

  bool UpdateMediaFormats(const OpalMediaFormat& input, const OpalMediaFormat& output) override {
    if (!OpalPluginFramedAudioTranscoder::UpdateMediaFormats(input, output))
      return false;
    PWaitAndSignal lock(updateMutex);
    const auto& raw = isEncoder ? inputMediaFormat : outputMediaFormat;
    const unsigned channels = OpalPluginCodecHandler::GetChannelCount(codecDef);
    if (raw.GetClockRate() != codecDef->sampleRate || raw.GetOptionInteger(OpalAudioFormat::ChannelsOption(), 1) != channels)
      return false;
    // AAC-LC frames span 1024 samples. OPAL's integer microsecond LCM with
    // its 1 ms PCM frames expands 21.333 ms into 21.333 s at 48 kHz.
    // The codec API supplies the exact sample and packet bounds instead.
    const PINDEX pcmBytes = codecDef->parm.audio.samplesPerFrame * channels * sizeof(short);
    const PINDEX packetBytes = codecDef->parm.audio.bytesPerFrame;
    inputBytesPerFrame = isEncoder ? pcmBytes : packetBytes;
    outputBytesPerFrame = isEncoder ? packetBytes : pcmBytes;
    maxOutputDataSize = outputBytesPerFrame;
    return true;
  }
};

class AacCodecHandler : public OpalPluginCodecHandler {
  PCLASSINFO(AacCodecHandler, OpalPluginCodecHandler);
public:
  void RegisterAudioTranscoder(const PString& source, const PString& destination,
                              const PluginCodec_Definition* codec, bool encoder) override {
    OpalPluginTranscoderFactory<AacFramedTranscoder>::Register(OpalTranscoderKey(source, destination), codec, encoder);
  }
};

class AacLcCapability : public H323CodecPluginGenericAudioCapability {
  PCLASSINFO(AacLcCapability, H323CodecPluginGenericAudioCapability);
public:
  explicit AacLcCapability(const H323CodecPluginGenericAudioCapability& original)
    : H323CodecPluginGenericAudioCapability(original) {}
  PObject* Clone() const override { return new AacLcCapability(*this); }
  PBoolean IsMatch(const PASN_Object& pdu, const PString& packetization) const override {
    if (!H323CodecPluginGenericAudioCapability::IsMatch(pdu, packetization))
      return false;
    const auto* audio = dynamic_cast<const H245_AudioCapability*>(&pdu);
    if (!audio || audio->GetTag() != H245_AudioCapability::e_genericAudioCapability)
      return false;
    const H245_GenericCapability& generic = *audio;
    bool hasAsc = false;
    if (generic.HasOptionalField(H245_GenericCapability::e_nonCollapsing))
      for (PINDEX index=0; index<generic.m_nonCollapsing.GetSize(); ++index) {
        const auto& parameter = generic.m_nonCollapsing[index];
        const auto& identifier = parameter.m_parameterIdentifier;
        if (identifier.GetTag() == H245_ParameterIdentifier::e_standard &&
            static_cast<const PASN_Integer&>(identifier) == 4) {
          if (hasAsc || parameter.m_parameterValue.GetTag() != H245_ParameterValue::e_octetString)
            return false;
          PBYTEArray expected;
          const PASN_OctetString& received = parameter.m_parameterValue;
          if (!GetMediaFormat().GetOptionOctets("AAC AudioSpecificConfig", expected) ||
              expected.GetSize() != received.GetSize() ||
              std::memcmp(expected.GetPointer(), static_cast<const BYTE*>(received), expected.GetSize()) != 0)
            return false;
          hasAsc = true;
        }
      }
    // TCS excludes ASC, so the otherwise identical 32/48 kHz capabilities
    // must resolve to our supported 48 kHz default. A 32 kHz OLC requires ASC.
    if (!hasAsc)
      return GetFormatName().Find("-32k-") == P_MAX_INDEX;
    // Stock OPAL checks generic options as TCS even for an OLC, ignores ASC,
    // and stops at its first rate match before final validation. Validate
    // OLC options now so FindCapability can continue to the correct rate.
    return true;
  }
};
}

bool RegisterMyStudioOpalAacHandler() {
  // OPAL deletes each returned handler after registration, so this factory
  // must produce a fresh instance rather than own a singleton handler.
  static PFactory<OpalPluginCodecHandler>::Worker<AacCodecHandler> worker("mystudio_opal_aac", false);
  return PFactory<OpalPluginCodecHandler>::IsRegistered("mystudio_opal_aac");
}

bool RegisterMyStudioOpalAacCapabilities() {
  for (const char* name : {"MyStudio-AAC-LC-48k-stereo192", "MyStudio-AAC-LC-48k-mono96",
                           "MyStudio-AAC-LC-32k-stereo192", "MyStudio-AAC-LC-32k-mono96"}) {
    std::unique_ptr<H323Capability> original(H323Capability::Create(name));
    const auto* generic = dynamic_cast<const H323CodecPluginGenericAudioCapability*>(original.get());
    if (!generic)
      return false;
    auto* replacement = new AacLcCapability(*generic);
    H323CapabilityFactory::Unregister(name);
    H323CapabilityFactory::Register(name, replacement);
  }
  return true;
}
