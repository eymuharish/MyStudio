#include <ptlib.h>
#include "OpalAacHandler.h"
#include <ptlib/pprocess.h>
#include <codec/opalpluginmgr.h>
#include <codec/opalplugin.h>
#include <asn/h245.h>
#include <h323/h323caps.h>
#include <opal/transcoders.h>
#include <sdp/sdp.h>
#include "../AudioPreferences.h"
#include "AacSdpSessionDescription.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
class TestProcess : public PProcess {
  PCLASSINFO(TestProcess, PProcess);
public:
  TestProcess() : PProcess("MyStudio", "OpalAacCoreTests", 1, 0, ReleaseCode, 0) {}
  void Main() override {}
};
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void H245Compatibility() {
  std::unique_ptr<H323Capability> receiver(H323Capability::Create("MyStudio-AAC-LD-LATM-64"));
  std::unique_ptr<H323Capability> peer(H323Capability::Create("MyStudio-AAC-LD-LATM-64"));
  auto* generic = dynamic_cast<H323GenericAudioCapability*>(peer.get());
  Require(receiver && generic, "AAC H.245 generic capability was not registered");
  H245_AudioCapability pdu;
  Require(generic->OnSendingPDU(pdu, 1, H323Capability::e_TCS) && receiver->IsMatch(pdu, PString::Empty()), "Supported Cisco H.245 capability did not match");
  const H245_GenericCapability& capability = pdu;
  Require(capability.m_nonCollapsing.GetSize() >= 3, "Cisco non-collapsing H.245 parameters missing");
  unsigned previous = 0;
  for (PINDEX index=0; index<capability.m_nonCollapsing.GetSize(); ++index) {
    const auto& identifier = capability.m_nonCollapsing[index].m_parameterIdentifier;
    Require(identifier.GetTag() == H245_ParameterIdentifier::e_standard, "AAC generic parameter must use standard ordinal");
    const unsigned ordinal = static_cast<const PASN_Integer&>(identifier);
    Require(index == 0 || ordinal > previous, "H.245 generic parameters are not in canonical ordinal order");
    previous = ordinal;
  }
  auto setPeerInteger = [&](unsigned ordinal, unsigned value) {
    H245_GenericCapability& mutableCapability = pdu;
    for (PINDEX index=0; index<mutableCapability.m_nonCollapsing.GetSize(); ++index) {
      auto& parameter = mutableCapability.m_nonCollapsing[index];
      if (static_cast<const PASN_Integer&>(parameter.m_parameterIdentifier) == ordinal) {
        static_cast<PASN_Integer&>(parameter.m_parameterValue) = value;
        return;
      }
    }
    throw std::runtime_error("Required AAC peer generic parameter is absent");
  };
  setPeerInteger(0, 25);
  Require(!receiver->IsMatch(pdu, PString::Empty()), "Cisco profile25 must not select fixed64 capability");
  setPeerInteger(0, 24);
  setPeerInteger(3, 2);
  Require(!receiver->IsMatch(pdu, PString::Empty()), "AAC-LC object2 must not select AAC-LD capability");
  std::cout << "H.245 generic ordering and incompatible operating-point rejection passed\n";
}
void H245LcRateSelection() {
  for (const char* layout : {"stereo192", "mono96"}) {
    const std::string rate48 = std::string("MyStudio-AAC-LC-48k-") + layout;
    const std::string rate32 = std::string("MyStudio-AAC-LC-32k-") + layout;
    H323Capabilities receivers;
    receivers.Add(H323Capability::Create(rate32.c_str()));
    receivers.Add(H323Capability::Create(rate48.c_str()));
    std::unique_ptr<H323Capability> peer(H323Capability::Create(rate48.c_str()));
    H245_Capability tcs;
    Require(peer && peer->OnSendingPDU(tcs), "LC48 TCS encoding failed");
    const auto* matched = receivers.FindCapability(tcs);
    Require(matched && matched->GetFormatName() == rate48.c_str(), "LC TCS without ASC selected32k instead of default48k");
    for (const auto& name : {rate48, rate32}) {
      peer.reset(H323Capability::Create(name.c_str()));
      H245_DataType olc;
      Require(peer && peer->OnSendingPDU(olc), "LC OLC encoding failed");
      matched = receivers.FindCapability(olc);
      Require(matched && matched->GetFormatName() == name.c_str(), "LC OLC ASC failed to select the exact sample rate/layout");
      OpalMediaFormat selected = matched->GetMediaFormat();
      selected.SetOptionString(OpalMediaFormat::ProtocolOption(), "H.323");
      const unsigned rate = name == rate48 ? 48000 : 32000;
      std::unique_ptr<OpalTranscoder> decoder(OpalTranscoder::Create(selected, GetOpalPCM16(rate, selected.GetOptionInteger(OpalAudioFormat::ChannelsOption(), 1))));
      Require(decoder != nullptr, "LC ASC-selected OLC cannot create its decoder");
      H245_AudioCapability& audio = olc;
      H245_GenericCapability& generic = audio;
      bool changed = false;
      for (PINDEX index=0; index<generic.m_nonCollapsing.GetSize(); ++index) {
        auto& parameter = generic.m_nonCollapsing[index];
        if (parameter.m_parameterIdentifier.GetTag() == H245_ParameterIdentifier::e_standard &&
            static_cast<const PASN_Integer&>(parameter.m_parameterIdentifier) == 4) {
          const BYTE invalid[] = {0, 0};
          // PASN SetValue(data,len) can reuse an equal-sized shared PBYTEArray
          // from OnSendingPDU. Assign independent bytes as a received PDU has.
          static_cast<PASN_OctetString&>(parameter.m_parameterValue).SetValue(PBYTEArray(invalid, sizeof(invalid)));
          changed = true;
        }
      }
      const auto* invalidMatch = receivers.FindCapability(olc);
      Require(changed && invalidMatch == nullptr, "LC OLC accepted an unsupported ASC");
    }
  }
  std::cout << "H.245 LC default48 TCS and exact32/48 OLC ASC selection passed\n";
}
void TranscoderRoundtrip(const char* name, unsigned rate, unsigned channels, unsigned samples, const char* protocol) {
  OpalMediaFormat format(name), raw(GetOpalPCM16(rate, channels));
  Require(format.IsValid() && raw.IsValid(), "AAC or its raw PCM format was not registered");
  format.SetOptionString(OpalMediaFormat::ProtocolOption(), protocol);
  raw.SetOptionString(OpalMediaFormat::ProtocolOption(), protocol);
  OpalMediaFormatList rawFormats, encodedFormats, masterFormats;
  rawFormats += raw; encodedFormats += format; masterFormats += raw; masterFormats += format;
  OpalMediaFormat selectedRaw, selectedEncoded;
  Require(OpalTranscoder::SelectFormats(OpalMediaType::Audio(), rawFormats, encodedFormats, masterFormats, selectedRaw, selectedEncoded), "OPAL could not select AAC encoder through PCM format merge");
  Require(selectedRaw.GetClockRate() == rate && selectedEncoded.GetClockRate() == format.GetClockRate() && selectedEncoded.GetFrameTime() == format.GetFrameTime(), "AAC selection changed PCM/RTP clocks or frame duration");
  OpalMediaFormat selectedInput, selectedOutput;
  Require(OpalTranscoder::SelectFormats(OpalMediaType::Audio(), encodedFormats, rawFormats, masterFormats, selectedInput, selectedOutput), "OPAL could not select AAC decoder through PCM format merge");
  Require(selectedEncoded.GetOptionInteger(OpalMediaFormat::TargetBitRateOption()) == format.GetOptionInteger(OpalMediaFormat::TargetBitRateOption()) &&
          selectedInput.GetOptionInteger(OpalMediaFormat::TargetBitRateOption()) == format.GetOptionInteger(OpalMediaFormat::TargetBitRateOption()), "AAC raw merge replaced the fixed encoded target bitrate with PCM bitrate");
  Require(selectedInput.GetOptionString(OpalMediaFormat::ProtocolOption()) == protocol && selectedEncoded.GetOptionString(OpalMediaFormat::ProtocolOption()) == protocol, "AAC selection lost its stamped protocol");
  std::unique_ptr<OpalTranscoder> encoder(OpalTranscoder::Create(selectedRaw, selectedEncoded));
  std::unique_ptr<OpalTranscoder> decoder(OpalTranscoder::Create(selectedInput, selectedOutput));
  Require(encoder && decoder, "OPAL failed to instantiate AAC encoder/decoder");
  Require(encoder->UpdateMediaFormats(selectedRaw, selectedEncoded) && decoder->UpdateMediaFormats(selectedInput, selectedOutput), "AAC format update failed");
  std::cout << name << " " << protocol << " PCM input bytes=" << encoder->GetOptimalDataFrameSize(true)
            << " expected=" << samples*channels*2 << '\n';
  Require(encoder->GetOptimalDataFrameSize(true) == samples*channels*2, "OPAL requested an incorrect PCM frame size for AAC");
  bool signal = false;
  for (unsigned block=0; block<12; ++block) {
    RTP_DataFrame input(samples*channels*2);
    auto* pcm = reinterpret_cast<std::int16_t*>(input.GetPayloadPtr());
    for (unsigned sample=0; sample<samples*channels; ++sample)
      pcm[sample] = static_cast<std::int16_t>(std::sin((sample + block*samples*channels)*0.071)*12000.0);
    input.SetTimestamp(block*samples);
    RTP_DataFrameList packets, output;
    Require(encoder->ConvertFrames(input, packets) && packets.GetSize() == 1, "OPAL did not produce one complete AAC packet");
    Require(packets[0].GetTimestamp() == static_cast<RTP_Timestamp>(static_cast<std::uint64_t>(block)*samples*format.GetClockRate()/rate), "OPAL AAC RTP timestamp uses incorrect clock");
    Require(decoder->ConvertFrames(packets[0], output) && output.GetSize() == 1, "OPAL did not decode its AAC packet");
    Require(output[0].GetTimestamp() == input.GetTimestamp(), "OPAL AAC decode did not restore PCM timestamp");
    Require(output[0].GetPayloadSize() == input.GetPayloadSize(), "OPAL AAC decoded PCM frame size differs from input");
    const auto* decoded = reinterpret_cast<const std::int16_t*>(output[0].GetPayloadPtr());
    signal = signal || std::any_of(decoded, decoded + samples*channels, [](auto value) { return std::abs(static_cast<int>(value)) > 100; });
  }
  Require(signal, "OPAL AAC produced no decoded signal");
  std::cout << name << " " << protocol << " core PCM roundtrip passed\n";
}
void UnstampedMasterIsRejected(const char* name, const char* protocol) {
  OpalMediaFormat peer(name), raw(GetOpalPCM16(48000, peer.GetOptionInteger(OpalAudioFormat::ChannelsOption(), 1)));
  peer.SetOptionString(OpalMediaFormat::ProtocolOption(), protocol);
  OpalMediaFormatList peers, raws, master;
  peers += peer; raws += raw; master += OpalMediaFormat(name); master += raw;
  OpalMediaFormat selectedInput, selectedOutput;
  Require(OpalTranscoder::SelectFormats(OpalMediaType::Audio(), peers, raws, master, selectedInput, selectedOutput), "Unstamped AAC master selection failed unexpectedly");
  Require(selectedInput.GetOptionString(OpalMediaFormat::ProtocolOption()).IsEmpty(), "Stock OPAL unexpectedly inherited protocol through NoMerge metadata");
  std::unique_ptr<OpalTranscoder> decoder(OpalTranscoder::Create(selectedInput, selectedOutput));
  Require(!decoder, "AAC must reject an unspecified protocol instead of guessing the packet container");
  std::cout << name << " " << protocol << " unstamped master rejected before codec use\n";
}
void LatmLateJoinAndReopen(const char* name, unsigned channels, const char* protocol) {
  OpalMediaFormat encoded(name), raw(GetOpalPCM16(48000, channels));
  encoded.SetOptionString(OpalMediaFormat::ProtocolOption(), protocol);
  raw.SetOptionString(OpalMediaFormat::ProtocolOption(), protocol);
  std::unique_ptr<OpalTranscoder> encoder(OpalTranscoder::Create(raw, encoded)), decoder;
  Require(encoder != nullptr, "Late-join LATM encoder creation failed");
  bool recovered = false;
  for (unsigned block=0; block<48; ++block) {
    RTP_DataFrame input(960*channels*2);
    auto* pcm = reinterpret_cast<std::int16_t*>(input.GetPayloadPtr());
    for (unsigned sample=0; sample<960*channels; ++sample)
      pcm[sample] = static_cast<std::int16_t>(std::sin((sample + block*960*channels)*0.071)*12000.0);
    input.SetTimestamp(block*960);
    RTP_DataFrameList packets, output;
    Require(encoder->ConvertFrames(input, packets) && packets.GetSize() == 1, "Late-join LATM encode failed");
    // Lose the initial mux config, then reopen the receiver while the same
    // remote encoder continues. Its later packets use the previous mux config.
    if (block == 3 || block == 23) {
      if (block == 23) Require(recovered, "Late-join LATM never recovered decoded PCM");
      decoder.reset(OpalTranscoder::Create(encoded, raw));
      Require(decoder != nullptr, "Late-join LATM decoder recreation failed");
      recovered = false;
    }
    if (!decoder) continue;
    Require(decoder->ConvertFrames(packets[0], output), "LATM packet before next mux config fatally stopped the OPAL receive patch");
    if (output.GetSize() && output[0].GetPayloadSize()) {
      Require(output[0].GetPayloadSize() == input.GetPayloadSize(), "Recovered LATM frame size is incorrect");
      const auto* decoded = reinterpret_cast<const std::int16_t*>(output[0].GetPayloadPtr());
      recovered = recovered || std::any_of(decoded, decoded + 960*channels, [](auto value) { return std::abs(static_cast<int>(value)) > 100; });
    }
  }
  Require(recovered, "Reopened LATM decoder never recovered decoded PCM");
  std::cout << name << " " << protocol << " lost initial mux config and reopened decoder recovered PCM\n";
}
void LatmSipPacketContract() {
  OpalMediaFormat sip("MyStudio-AAC-LD-LATM-64"), h323(sip), raw(GetOpalPCM16(48000, 1));
  sip.SetOptionString(OpalMediaFormat::ProtocolOption(), "SIP");
  h323.SetOptionString(OpalMediaFormat::ProtocolOption(), "H.323");
  // RFC6416 section6.1 and the existing Cisco factory map AudioMuxElement
  // directly to RTP payload. H.323's raw FDK output is an independent fixture
  // for SIP reception; comparing two SIP implementations would hide a wrapper.
  std::unique_ptr<OpalTranscoder> sipEncoder(OpalTranscoder::Create(raw, sip)), rawEncoder(OpalTranscoder::Create(raw, h323));
  std::unique_ptr<OpalTranscoder> sipDecoder(OpalTranscoder::Create(sip, raw)), rawDecoder(OpalTranscoder::Create(h323, raw));
  Require(sipEncoder && rawEncoder && sipDecoder && rawDecoder, "LATM wire-contract codecs could not be created");
  bool signal = false;
  for (unsigned block=0; block<12; ++block) {
    RTP_DataFrame input(1920);
    auto* pcm = reinterpret_cast<std::int16_t*>(input.GetPayloadPtr());
    for (unsigned sample=0; sample<960; ++sample)
      pcm[sample] = static_cast<std::int16_t>(std::sin((sample + block*960)*0.071)*12000.0);
    input.SetTimestamp(block*960);
    RTP_DataFrameList sipPackets, rawPackets, sipOutput, rawOutput;
    Require(sipEncoder->ConvertFrames(input, sipPackets) && rawEncoder->ConvertFrames(input, rawPackets), "LATM wire-contract encode failed");
    Require(sipPackets.GetSize() == 1 && rawPackets.GetSize() == 1 &&
            sipPackets[0].GetPayloadSize() == rawPackets[0].GetPayloadSize() &&
            std::memcmp(sipPackets[0].GetPayloadPtr(), rawPackets[0].GetPayloadPtr(), rawPackets[0].GetPayloadSize()) == 0,
            "MP4A-LATM SIP added bytes before the raw AudioMuxElement");
    rawPackets[0].SetMarker(false); // Cisco sends complete20ms units with M=0.
    Require(sipDecoder->ConvertFrames(rawPackets[0], sipOutput) && sipOutput.GetSize() == 1 && sipOutput[0].GetPayloadSize() == 1920,
            "SIP AAC decoder rejected direct Cisco-style AudioMuxElement");
    Require(rawDecoder->ConvertFrames(sipPackets[0], rawOutput) && rawOutput.GetSize() == 1 && rawOutput[0].GetPayloadSize() == 1920,
            "SIP AAC output cannot be decoded as a raw AudioMuxElement");
    const auto* decoded = reinterpret_cast<const std::int16_t*>(sipOutput[0].GetPayloadPtr());
    signal = signal || std::any_of(decoded, decoded + 960, [](auto value) { return std::abs(static_cast<int>(value)) > 100; });
  }
  Require(signal, "Direct LATM SIP fixture produced no decoded signal");
  std::cout << "SIP LATM direct AudioMuxElement contract and independent decoded PCM passed\n";
}
void CiscoAnswerCpresentDefault() {
  // Audio section reproduced from the SX20 TC7.3.3 answer; addresses/session
  // identifiers are local fixtures. Cisco omits cpresent and /1 in rtpmap.
  const PString answer =
    "v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=Cisco SX20\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\n"
    "m=audio 2356 RTP/AVP 99 9 101\r\na=sendrecv\r\n"
    "a=rtpmap:99 MP4A-LATM/90000\r\n"
    "a=fmtp:99 profile-level-id=24;object=23;bitrate=64000\r\n"
    "a=rtpmap:9 G722/8000\r\na=rtpmap:101 telephone-event/8000\r\na=ptime:20\r\n";
  OpalMediaFormat ld("MyStudio-AAC-LD-LATM-64");
  OpalMediaFormatList supported; supported += ld;
  SDPSessionDescription decoded(0, 0, OpalTransportAddress("udp$127.0.0.1:5000"));
  Require(decoded.Decode(answer, supported) && decoded.GetMediaFormats().HasFormat(ld.GetName()), "Cisco SX20 answer omitted cpresent and lost supported AAC-LD");
  OpalMediaFormat negotiated = *decoded.GetMediaFormats().FindFormat(ld.GetName());
  Require(negotiated.GetOptionBoolean("AAC mux config present") && negotiated.GetClockRate() == 90000 && negotiated.GetPayloadType() == 99,
          "Omitted cpresent must retain in-band configuration and RTP90k/99");
  negotiated.SetOptionString(OpalMediaFormat::ProtocolOption(), "SIP");
  std::unique_ptr<OpalTranscoder> encoder(OpalTranscoder::Create(GetOpalPCM16(48000, 1), negotiated));
  std::unique_ptr<OpalTranscoder> decoder(OpalTranscoder::Create(negotiated, GetOpalPCM16(48000, 1)));
  Require(encoder && decoder, "Cisco cpresent-default answer cannot instantiate AAC codecs");
  for (const char* incompatible : {";cpresent=0"}) {
    PString invalid = answer; invalid.Replace("bitrate=64000", PString("bitrate=64000") + incompatible);
    SDPSessionDescription rejected(0, 0, OpalTransportAddress("udp$127.0.0.1:5000"));
    Require(rejected.Decode(invalid, supported) && !rejected.GetMediaFormats().HasFormat(ld.GetName()), "Cisco AAC-LD accepted explicit unsupported cpresent");
  }
  std::cout << "Cisco SX20 omitted cpresent defaults1; explicit0 rejected\n";
}
void CiscoOmittedStereoChannels(){
 OpalMediaFormat stereo("MyStudio-AAC-LD-LATM-128"),mono("MyStudio-AAC-LD-LATM-64");
 OpalMediaFormatList supported;supported+=stereo;supported+=mono;supported+=OpalG722;
 const PString prefix="v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=Owned SX20 channel fixture\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\nm=audio 2358 RTP/AVP 100 107 9\r\n";
 const PString suffix="a=fmtp:100 profile-level-id=25;object=23;bitrate=128000\r\na=rtpmap:107 MP4A-LATM/90000/1\r\na=fmtp:107 profile-level-id=24;object=23;bitrate=64000\r\na=rtpmap:9 G722/8000\r\n";
 studio::opal::AacSdpSessionDescription decoded(1,1,OpalTransportAddress("udp$127.0.0.1"));
 Require(decoded.Decode(prefix+"a=rtpmap:100 MP4A-LATM/90000\r\n"+suffix,supported),"Actual omitted-channel SDP decode failed");
 auto formats=decoded.GetMediaFormats();auto found=formats.FindFormat(stereo.GetName());
 Require(found!=formats.end(),"Omitted-channel stereo profile disappeared");auto negotiated=*found;
 Require(negotiated.GetOptionInteger(OpalAudioFormat::ChannelsOption(),1)==2,"Omitted rtpmap channels overrode exact supported AAC-LD stereo profile");
 negotiated.SetOptionString(OpalMediaFormat::ProtocolOption(),"SIP");auto raw=GetOpalPCM16(48000,2);
 std::unique_ptr<OpalTranscoder> encoder(OpalTranscoder::Create(raw,negotiated)),decoder(OpalTranscoder::Create(negotiated,raw));
 Require(encoder&&decoder,"Omitted-channel negotiated stereo cannot construct its real encoder/decoder");
 unsigned nonzero=0;
 for(unsigned block=0;block<20;++block){RTP_DataFrame pcm(960*2*sizeof(short));auto* samples=reinterpret_cast<short*>(pcm.GetPayloadPtr());for(unsigned i=0;i<1920;++i)samples[i]=short(12000*std::sin((i+block*1920)*0.071));pcm.SetTimestamp(block*1800);RTP_DataFrameList packets,output;Require(encoder->ConvertFrames(pcm,packets)&&!packets.IsEmpty(),"Negotiated omitted-channel stereo encoder failed");for(auto& packet:packets){Require(decoder->ConvertFrames(packet,output),"Negotiated omitted-channel decoder failed");for(auto& frame:output){Require(frame.GetPayloadSize()==pcm.GetPayloadSize(),"Negotiated stereo decoder returned mono PCM");auto* values=reinterpret_cast<const short*>(frame.GetPayloadPtr());for(unsigned i=0;i<frame.GetPayloadSize()/sizeof(short);++i)nonzero+=std::abs(int(values[i]))>100;}}}
 Require(nonzero>0,"Negotiated omitted-channel stereo produced no decoded audio");
 for(const char* mapping:{"100 MP4A-LATM/90000  ","100 MP4A-LATM/90000\t","100\tMP4A-LATM/90000"}){
  studio::opal::AacSdpSessionDescription whitespace(1,1,OpalTransportAddress("udp$127.0.0.1"));
  Require(whitespace.Decode(prefix+"a=rtpmap:"+PString(mapping)+"\r\n"+suffix,supported),"Whitespace AAC SDP decode failed");
  auto whitespaceFormats=whitespace.GetMediaFormats();auto candidate=whitespaceFormats.FindFormat(stereo.GetName());
  Require(candidate!=whitespaceFormats.end()&&candidate->GetOptionInteger(OpalAudioFormat::ChannelsOption(),1)==2,"Rtpmap whitespace misplaced inferred stereo channels");
 }
 studio::opal::AacSdpSessionDescription explicitMono(1,1,OpalTransportAddress("udp$127.0.0.1"));
 Require(explicitMono.Decode(prefix+"a=rtpmap:100 MP4A-LATM/90000/1\r\n"+suffix,supported),"Explicit mono SDP failed to decode");
 auto monoFormats=explicitMono.GetMediaFormats();auto explicitStereo=monoFormats.FindFormat(stereo.GetName());
 if(explicitStereo!=monoFormats.end())Require(explicitStereo->GetOptionInteger(OpalAudioFormat::ChannelsOption(),2)==1,"Explicit mono was silently promoted to stereo");
 Require(!monoFormats.HasFormat(stereo.GetName()),"Explicit /1 left an unusable stereo codec ahead of mono fallback");
 Require(!monoFormats.IsEmpty()&&monoFormats.front().GetName()==mono.GetName(),"Explicit /1 did not select the first supported mono fallback");
 auto selectedMono=monoFormats.FindFormat(mono.GetName());Require(selectedMono!=monoFormats.end(),"Explicit mono fallback disappeared");auto monoFormat=*selectedMono;monoFormat.SetOptionString(OpalMediaFormat::ProtocolOption(),"SIP");std::unique_ptr<OpalTranscoder> monoEncoder(OpalTranscoder::Create(GetOpalPCM16(48000,1),monoFormat));Require(monoEncoder&&monoFormat.GetOptionInteger(OpalAudioFormat::ChannelsOption(),2)==1,"Explicit mono fallback cannot construct mono encoder");
 for(const char* extra:{";config=400026103fc0",";config=xyz",";object=23"}){
  PString guardedSuffix=suffix;guardedSuffix.Replace("bitrate=128000","bitrate=128000"+PString(extra));
  studio::opal::AacSdpSessionDescription guarded(1,1,OpalTransportAddress("udp$127.0.0.1"));
  Require(guarded.Decode(prefix+"a=rtpmap:100 MP4A-LATM/90000\r\n"+guardedSuffix,supported),"Guarded AAC SDP decode failed");
  Require(!guarded.GetMediaFormats().HasFormat(stereo.GetName()),"Unvalidated config or duplicate AAC parameter was normalized to stereo");
  Require(guarded.GetMediaFormats().HasFormat(mono.GetName()),"Refusing stereo inference damaged supported mono fallback");
 }
 std::cout<<"SX20 omitted-channel stereo actual SDP/codec PCM PASS nonzero="<<nonzero<<" explicit mono preserved\n";
}
void StereoMonoG722SdpFallback(const char* endpoint,const char* stereoName,const char* monoName) {
  telestudio::media::AudioNetworkProfile profile;
  auto names=studio::opal::AudioOrder(true,true,endpoint,profile);
  OpalMediaFormatList supported; PStringArray order;
  for(const auto& name:names) {
    OpalMediaFormat format(name.c_str());
    if(format.IsValid()&&format.GetMediaType()==OpalMediaType::Audio()) {
      format.SetOptionString(OpalMediaFormat::ProtocolOption(),"SIP"); supported+=format; order.AppendString(name.c_str());
    }
  }
  for(int mode=0;mode<3;++mode) {
    SDPSessionDescription offered(1,1,OpalTransportAddress("udp$127.0.0.1:5000"));
    auto* media=new SDPAudioMediaDescription(OpalTransportAddress("udp$127.0.0.1:5000"));
    // Deliberately list mono first in a dual-capability peer. Local preference
    // should still select stereo, while mono-only peers must stay mono.
    if(mode<2) media->AddMediaFormat(OpalMediaFormat(monoName));
    if(mode==0) media->AddMediaFormat(OpalMediaFormat(stereoName));
    media->AddMediaFormat(OpalG722); offered.AddMediaDescription(media);
    SDPSessionDescription decoded(0,0,OpalTransportAddress("udp$127.0.0.1:5002"));
    Require(decoded.Decode(offered.Encode(),supported),"Stereo/mono/G722 peer SDP failed to parse");
    auto matches=decoded.GetMediaFormats();matches.Reorder(order);
    const auto* expected=mode==0?stereoName:mode==1?monoName:static_cast<const char*>(OpalG722.GetName());
    if(matches.GetSize()==0||matches[0].GetName()!=expected) {
      std::cout<<"Fallback fixture "<<endpoint<<" mode="<<mode<<" expected="<<expected<<" offered:\n"<<offered.Encode()<<"decoded:\n";
      for(const auto& match:matches) std::cout<<match.GetName()<<" channels="<<match.GetOptionInteger(OpalAudioFormat::ChannelsOption(),1)<<'\n';
    }
    Require(matches.GetSize()>0&&matches[0].GetName()==expected,"SDP capabilities failed stereo -> mono -> G722 fallback");
    auto selected=matches[0];const auto channels=selected.GetOptionInteger(OpalAudioFormat::ChannelsOption(),1);
    Require(channels==(mode==0?2:1),"SDP selected a channel layout the peer did not advertise");
    selected.SetOptionString(OpalMediaFormat::ProtocolOption(),"SIP");
    auto pcm=GetOpalPCM16(mode==2?16000:48000,channels);
    std::unique_ptr<OpalTranscoder> encoder(OpalTranscoder::Create(pcm,selected)),decoder(OpalTranscoder::Create(selected,pcm));
    Require(encoder&&decoder,"SDP-selected operating point cannot create send/receive codecs");
  }
  std::cout<<endpoint<<" SDP stereo preference, mono-only and G722-only fallback passed\n";
}
}
int main(int argc, char** argv) {
  try {
    std::cout << std::unitbuf;
    Require(argc == 3, "Pass AAC and G722 plugin DLL paths");
    Require(RegisterMyStudioOpalAacHandler(), "AAC exact frame handler registration failed");
    PPluginManager::GetPluginManager().SetDirectories(PString(argv[1]) + ".no-auto-plugins");
    TestProcess process;
    if (const auto* trace = std::getenv("ESSASTUDIO_DIAGNOSTIC_LOG"); trace && std::strcmp(trace, "1") == 0)
      PTrace::Initialise(4, nullptr, PTrace::Timestamp|PTrace::Thread|PTrace::FileAndLine);
    PDynaLink plugin(argv[1]);
    Require(plugin.IsLoaded(), "PTLib could not load AAC plugin DLL");
    PDynaLink g722(argv[2]);
    Require(g722.IsLoaded(), "PTLib could not load G722 fallback plugin DLL");
    OpalPluginCodecManager codecManager;
    codecManager.OnLoadPlugin(plugin, 0);
    codecManager.OnLoadPlugin(g722, 0);
    Require(RegisterMyStudioOpalAacCapabilities(), "AAC H.245 rate-aware capability registration failed");
    OpalMediaFormat ld("MyStudio-AAC-LD-LATM-64"), lc("MyStudio-AAC-LC-48k-stereo192");
    Require(ld.IsValid() && ld.GetClockRate() == 90000 && ld.GetFrameTime() == 1800, "Registered OPAL LD clock/frame are incorrect");
    Require(lc.IsValid() && lc.GetClockRate() == 48000 && lc.GetFrameTime() == 1024, "Registered OPAL LC clock/frame are incorrect");
    CiscoAnswerCpresentDefault();
    CiscoOmittedStereoChannels();
    StereoMonoG722SdpFallback("sx20","MyStudio-AAC-LD-LATM-128","MyStudio-AAC-LD-LATM-64");
    StereoMonoG722SdpFallback("pcs-xg77","MyStudio-AAC-LC-48k-stereo192","MyStudio-AAC-LC-48k-mono96");
    SDPSessionDescription sdp(1, 1, OpalTransportAddress("udp$127.0.0.1:5000"));
    auto* audio = new SDPAudioMediaDescription(OpalTransportAddress("udp$127.0.0.1:5000"));
    audio->AddMediaFormat(ld); audio->AddMediaFormat(lc); sdp.AddMediaDescription(audio);
    std::cout << sdp.Encode() << '\n';
    Require(sdp.Encode().Find("MP4A-LATM/90000") != P_MAX_INDEX, "Cisco SDP must use RTP 90000");
    Require(sdp.Encode().Find("MPEG4-GENERIC/48000/2") != P_MAX_INDEX, "Sony SDP must use 48000 stereo");
    OpalMediaFormatList supported; supported += ld; supported += lc;
    SDPSessionDescription decodedSdp(0, 0, OpalTransportAddress("udp$127.0.0.1:5000"));
    Require(decodedSdp.Decode(sdp.Encode(), supported), "OPAL failed to decode AAC SDP");
    const auto negotiated = decodedSdp.GetMediaFormats();
    Require(negotiated.HasFormat(ld.GetName()) && negotiated.HasFormat(lc.GetName()), "SDP encode/decode rejected its supported AAC format");
    for (auto format : negotiated) {
      format.SetOptionString(OpalMediaFormat::ProtocolOption(), "SIP");
      const auto raw = GetOpalPCM16(48000, format.GetOptionInteger(OpalAudioFormat::ChannelsOption(), 1));
      std::unique_ptr<OpalTranscoder> encoder(OpalTranscoder::Create(raw, format));
      std::unique_ptr<OpalTranscoder> decoder(OpalTranscoder::Create(format, raw));
      Require(encoder && decoder, "SDP-decoded AAC options prevented transcoder creation");
    }
    std::cout << "Real SDP encode/decode AAC merge and codec creation passed\n";
    auto rejectSdp = [&](const char* from, const char* to, const OpalMediaFormat& rejected) {
      PString invalidOffer = sdp.Encode(); invalidOffer.Replace(from, to, true);
      SDPSessionDescription invalidSdp(0, 0, OpalTransportAddress("udp$127.0.0.1:5000"));
      Require(invalidSdp.Decode(invalidOffer, supported) && !invalidSdp.GetMediaFormats().HasFormat(rejected.GetName()), "SDP accepted unsupported AAC operating point");
    };
    rejectSdp("profile-level-id=24", "profile-level-id=25", ld);
    rejectSdp("object=23", "object=2", ld);
    rejectSdp("bitrate=64000", "bitrate=128000", ld);
    rejectSdp("bitrate=64000", "bitrate=64000;cpresent=0", ld);
    rejectSdp("MP4A-LATM/90000", "MP4A-LATM/48000", ld);
    rejectSdp("mode=AAC-hbr", "mode=AAC-lbr", lc);
    rejectSdp("config=1190", "config=1290", lc);
    std::cout << "SDP rejects incompatible Cisco/Sony AAC operating points\n";
    H245Compatibility();
    H245LcRateSelection();
    LatmSipPacketContract();
    LatmLateJoinAndReopen("MyStudio-AAC-LD-LATM-64", 1, "SIP");
    LatmLateJoinAndReopen("MyStudio-AAC-LD-LATM-64", 1, "H.323");
    LatmLateJoinAndReopen("MyStudio-AAC-LD-LATM-128", 2, "H.323");
    for (const char* protocol : {"SIP", "H.323"}) {
      UnstampedMasterIsRejected("MyStudio-AAC-LD-LATM-64", protocol);
      UnstampedMasterIsRejected("MyStudio-AAC-LC-48k-stereo192", protocol);
      TranscoderRoundtrip("MyStudio-AAC-LD-LATM-64", 48000, 1, 960, protocol);
      TranscoderRoundtrip("MyStudio-AAC-LC-48k-stereo192", 48000, 2, 1024, protocol);
      TranscoderRoundtrip("MyStudio-AAC-LD-LATM-128", 48000, 2, 960, protocol);
      TranscoderRoundtrip("MyStudio-AAC-LC-48k-mono96", 48000, 1, 1024, protocol);
    }
    TranscoderRoundtrip("MyStudio-AAC-LD-LATM-128", 48000, 2, 960, "H.323");
    TranscoderRoundtrip("MyStudio-AAC-LC-48k-mono96", 48000, 1, 1024, "H.323");
    TranscoderRoundtrip("MyStudio-AAC-LC-32k-stereo192", 32000, 2, 1024, "H.323");
    TranscoderRoundtrip("MyStudio-AAC-LC-32k-mono96", 32000, 1, 1024, "H.323");
    std::cout << "OPAL registered AAC SDP and transcoders passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
