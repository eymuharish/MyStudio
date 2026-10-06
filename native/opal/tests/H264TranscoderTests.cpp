#include <ptlib.h>
#include <ptlib/pprocess.h>
#include <codec/vidcodec.h>
#include <opal/transcoders.h>
#include <telestudio/media/FrameConverter.h>
#include "../OpalH264.h"
#include <iostream>
#include <memory>
class TestProcess : public PProcess {
 PCLASSINFO(TestProcess,PProcess);
public:TestProcess():PProcess("MyStudio","OpalH264Tests",1,0,ReleaseCode,0){} void Main() override{}
};
int main(){_putenv_s("PTLIBPLUGINDIR","MyStudio-test-no-auto-plugins");TestProcess process;try{
 studio::opal::RegisterH264();
 std::unique_ptr<OpalTranscoder> enc(OpalTranscoder::Create(OpalYUV420P,OpalH264)),dec(OpalTranscoder::Create(OpalH264,OpalYUV420P));
 if(!enc||!dec)throw std::runtime_error("H264 native transcoders not registered");
 std::unique_ptr<OpalTranscoder> mode0enc(OpalTranscoder::Create(OpalYUV420P,OpalH264_MODE0)),mode0dec(OpalTranscoder::Create(OpalH264_MODE0,OpalYUV420P));
 if(!mode0enc||!mode0dec)throw std::runtime_error("Sony H264 mode0 transcoders not registered");
 const int w=320,h=240;RTP_DataFrame raw(sizeof(PluginCodec_Video_FrameHeader)+w*h*3/2);
 auto*head=reinterpret_cast<PluginCodec_Video_FrameHeader*>(raw.GetPayloadPtr());*head={0,0,w,h};
 std::fill_n(OpalVideoFrameDataPtr(head),w*h,90);std::fill_n(OpalVideoFrameDataPtr(head)+w*h,w*h/2,128);raw.SetTimestamp(9000);
 RTP_DataFrameList encoded,decoded;unsigned decodedCount=0,packetCount=0;WORD sequence=1;
 for(unsigned frame=0;frame<5;++frame){raw.SetTimestamp(9000+frame*3750);if(!enc->ConvertFrames(raw,encoded)||encoded.IsEmpty())throw std::runtime_error("No encoded H264 packets");packetCount+=encoded.GetSize();for(PINDEX i=0;i<encoded.GetSize();++i){encoded[i].SetSequenceNumber(sequence++);if(!dec->ConvertFrames(encoded[i],decoded))throw std::runtime_error("H264 decode rejected packet");decodedCount+=decoded.GetSize();}}
 if(!decodedCount)throw std::runtime_error("No decoded H264 frame");
 RTP_DataFrame malformed(2);malformed.GetPayloadPtr()[0]=28;malformed.GetPayloadPtr()[1]=0;malformed.SetMarker(true);malformed.SetTimestamp(18000);
 if(!dec->ConvertFrames(malformed,decoded)||!decoded.IsEmpty())throw std::runtime_error("Malformed FU-A produced video");
 const int sw=1280,sh=720;RTP_DataFrame sony(sizeof(PluginCodec_Video_FrameHeader)+sw*sh*3/2);auto*sonyHead=reinterpret_cast<PluginCodec_Video_FrameHeader*>(sony.GetPayloadPtr());*sonyHead={0,0,sw,sh};auto*sonyPixels=OpalVideoFrameDataPtr(sonyHead);for(int i=0;i<sw*sh;++i)sonyPixels[i]=BYTE((i*193+i/sw*37)%256);std::fill_n(sonyPixels+sw*sh,sw*sh/2,128);unsigned sonyDecoded=0,sonyPackets=0;
 for(unsigned frame=0;frame<4;++frame){sony.SetTimestamp(24000+frame*3000);if(!mode0enc->ConvertFrames(sony,encoded)||encoded.IsEmpty())throw std::runtime_error("No Sony mode0 packets");for(auto&packet:encoded){if(packet.GetPayloadSize()>1200||(packet.GetPayloadPtr()[0]&31)>=24)throw std::runtime_error("Mode0 emitted aggregation/fragmentation or oversize NAL");++sonyPackets;packet.SetSequenceNumber(sequence++);if(!mode0dec->ConvertFrames(packet,decoded))throw std::runtime_error("Sony mode0 decode failed");for(auto&frame:decoded){auto*h=reinterpret_cast<const PluginCodec_Video_FrameHeader*>(frame.GetPayloadPtr());if(h->width!=sw||h->height!=sh)throw std::runtime_error("Sony mode0 decoded dimensions differ");++sonyDecoded;}}}
 if(!sonyDecoded)throw std::runtime_error("Sony mode0 produced no decoded video");if(!mode0dec->ConvertFrames(malformed,decoded)||!decoded.IsEmpty())throw std::runtime_error("Mode0 accepted FU-A");
 std::cout<<"Sony H264 mode0 720p single-NAL PASS packets="<<sonyPackets<<" decoded="<<sonyDecoded<<'\n';
 std::cout<<"OPAL H264 roundtrip PASS packets="<<packetCount<<" decoded="<<decodedCount<<'\n';return 0;
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
