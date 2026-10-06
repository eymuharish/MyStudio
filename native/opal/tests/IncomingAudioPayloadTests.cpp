#include <ptlib.h>
#include <ptlib/pprocess.h>
#include <codec/opalpluginmgr.h>
#include <codec/vidcodec.h>
#include <opal/transcoders.h>
#include "../aac/OpalAacHandler.h"
#include "../IncomingAudioPayload.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>

class TestProcess : public PProcess {
 PCLASSINFO(TestProcess,PProcess);
public:TestProcess():PProcess("MyStudio","IncomingAudioPayload",1,0,ReleaseCode,0){}void Main()override{}
};
void Require(bool result,const char*message){if(!result)throw std::runtime_error(message);}

void NegotiatedPayloadClassification(){
 OpalMediaFormat source("MyStudio-AAC-LD-LATM-128"),matching(source),other("MyStudio-AAC-LC-48k-stereo192"),video(OpalH264);
 Require(source.IsValid()&&other.IsValid()&&video.IsValid(),"Negotiated mapping fixtures unavailable");
 source.SetPayloadType(static_cast<RTP_DataFrame::PayloadTypes>(120));
 for(auto*format:{&matching,&other,&video})format->SetPayloadType(static_cast<RTP_DataFrame::PayloadTypes>(102));
 OpalMediaFormatList formats;formats+=other;
 Require(studio::opal::HasKnownAudioPayloadConflict(source,102,formats),"Known other audio codec PT102 was not rejected");
 formats+=matching;
 Require(!studio::opal::HasKnownAudioPayloadConflict(source,102,formats),"Matching source PT102 lost to an unrelated audio format");
 OpalMediaFormatList reversed;reversed+=matching;reversed+=other;
 Require(!studio::opal::HasKnownAudioPayloadConflict(source,102,reversed),"Audio payload mapping depends on format order");
 OpalMediaFormatList videoOnly;videoOnly+=video;
 Require(!studio::opal::HasKnownAudioPayloadConflict(source,102,videoOnly),"Independent video PT102 contaminated audio namespace");
 std::cout<<"Actual SDP format classification PASS matching source precedence/audio-video namespaces\n";
}

void DynamicAudioRecovery() {
 // Both sides use a known, registered codec. PT102 alone identifies no codec.
 OpalMediaFormat encoded("MyStudio-AAC-LD-LATM-128"),raw(GetOpalPCM16(48000,2));
 encoded.SetOptionString(OpalMediaFormat::ProtocolOption(),"SIP");
 encoded.SetPayloadType(static_cast<RTP_DataFrame::PayloadTypes>(120));
 std::unique_ptr<OpalTranscoder> encoder(OpalTranscoder::Create(raw,encoded));
 std::unique_ptr<OpalTranscoder> decoder(OpalTranscoder::Create(encoded,raw));
 Require(encoder && decoder,"AAC fixture codecs unavailable");
 unsigned samples=0,nonzero=0,firstDecoded=0;
 auto receive=[&](unsigned block,unsigned wirePayload,bool conflict=false){
  RTP_DataFrame input(960*2*sizeof(short));auto*pcm=reinterpret_cast<std::int16_t*>(input.GetPayloadPtr());
  for(unsigned n=0;n<1920;++n)pcm[n]=static_cast<std::int16_t>(std::sin((n+block*1920)*0.071)*12000);
  input.SetTimestamp(block*960);RTP_DataFrameList packets,output;
  Require(encoder->ConvertFrames(input,packets)&&packets.GetSize()==1,"AAC encode fixture failed");
  packets[0].SetPayloadType(static_cast<RTP_DataFrame::PayloadTypes>(wirePayload));
  if(studio::opal::ShouldDropIncomingAudioPayload(packets[0].GetPayloadSize(),packets[0].GetPayloadType(),encoded.GetPayloadType(),conflict))packets[0].SetPayloadSize(0);
  Require(decoder->ConvertFrames(packets[0],output),"RTP audio decode fatally stopped");
  for(auto&frame:output){
   if(!frame.GetPayloadSize())continue;
   if(!firstDecoded)firstDecoded=block+1;
   auto*decoded=reinterpret_cast<const std::int16_t*>(frame.GetPayloadPtr());
   samples+=frame.GetPayloadSize()/sizeof(short);
   nonzero+=static_cast<unsigned>(std::count_if(decoded,decoded+frame.GetPayloadSize()/sizeof(short),[](auto sample){return std::abs(int(sample))>100;}));
  }
 };
 // Varying unexpected labels must never reach the negotiated AAC decoder.
 for(unsigned block=0;block<20;++block)receive(block,block%2?102:103);
 Require(samples==0&&decoder->GetInputFormat().GetPayloadType()==120,"Unstable dynamic labels bypassed SDK payload validation");
 // The last alternating packet already starts this run of PT102.
 for(unsigned block=20;block<100;++block){receive(block,102);if(block<30)Require(samples==0,"Dynamic mismatch decoded before SDK stability threshold");}
 Require(samples>0&&nonzero>0,"Dynamic RTP mismatch guard starved real AAC decoder: zero PCM");
 Require(firstDecoded>30&&firstDecoded<65,"Dynamic RTP bypass skipped SDK bounded recovery or never recovered");
 Require(decoder->GetInputFormat().GetPayloadType()==102&&encoded.GetPayloadType()==120,"Decoder recovery must learn wire PT while source format remains unchanged");
 const unsigned previouslyDecoded=samples;
 for(unsigned block=100;block<140;++block)receive(block,120);
 Require(samples>previouslyDecoded&&decoder->GetInputFormat().GetPayloadType()==120,"Return to original dynamic payload never recovered");
 const unsigned beforeConflict=samples;
 for(unsigned block=140;block<160;++block)receive(block,102,true);
 Require(samples==beforeConflict&&decoder->GetInputFormat().GetPayloadType()==120,"Known codec conflict reached or remapped real AAC decoder");
 std::cout<<"Dynamic120->102->120 real AAC recovery PASS firstDecoded="<<firstDecoded<<" samples="<<samples<<" nonzero="<<nonzero<<'\n';
}
int main(int argc,char**argv){try{
 Require(argc==2,"Pass AAC plugin DLL path");Require(RegisterMyStudioOpalAacHandler(),"AAC handler registration failed");
 _putenv_s("PTLIBPLUGINDIR","MyStudio-no-auto-plugins");TestProcess process;
 PDynaLink plugin(argv[1]);Require(plugin.IsLoaded(),"AAC plugin DLL load failed");OpalPluginCodecManager codecs;codecs.OnLoadPlugin(plugin,0);
 NegotiatedPayloadClassification();
 DynamicAudioRecovery();
 for(unsigned pt:{9u,102u,120u})Require(!studio::opal::ShouldDropIncomingAudioPayload(80,pt,pt),"Exact payload was dropped");
 Require(!studio::opal::ShouldDropIncomingAudioPayload(0,102,9),"Empty jitter frame was dropped");
 Require(!studio::opal::ShouldDropIncomingAudioPayload(1,13,120),"Comfort noise handling changed");
 Require(studio::opal::ShouldDropIncomingAudioPayload(80,0,9),"Wrong static codec was admitted");
 Require(studio::opal::ShouldDropIncomingAudioPayload(80,102,9),"Dynamic packet was admitted to static G722");
 Require(studio::opal::ShouldDropIncomingAudioPayload(80,9,120),"Static packet was admitted to dynamic codec");
 Require(studio::opal::ShouldDropIncomingAudioPayload(80,102,120,true),"Known different codec was admitted to negotiated decoder");
 Require(studio::opal::ShouldDropIncomingAudioPayload(80,128,120),"Invalid RTP payload was admitted");
 std::cout<<"Exact/static/CN/empty audio guard PASS\n";return 0;
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
