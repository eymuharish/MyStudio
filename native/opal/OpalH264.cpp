#include <ptlib.h>
#include <codec/vidcodec.h>
#include "OpalH264.h"
#include <telestudio/media/IVideoEncoder.h>
#include <telestudio/media/FfmpegVideoDecoder.h>
#include <telestudio/media/FrameConverter.h>
#include <telestudio/media/RtpPacketizer.h>
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <atomic>
namespace telestudio {std::unique_ptr<IVideoEncoder> CreateOpalX264Encoder(const std::filesystem::path&,unsigned maxNalBytes);}
namespace studio::opal {
static std::mutex observerMutex;
static VideoAcceptance observer;
static std::atomic<bool> textContent{true};
void SetVideoContentMode(bool text){textContent=text;}
void SetVideoAcceptance(VideoAcceptance value){std::scoped_lock lock(observerMutex);observer=std::move(value);}
static void Observe(const RTP_DataFrame&f,const OpalMediaFormat&format,bool accepted,int w,int h,int rate){VideoAcceptance callback;{std::scoped_lock lock(observerMutex);callback=observer;}if(callback)callback(f.GetTimestamp(),format.GetOptionEnum(OpalVideoFormat::ContentRoleOption(),OpalVideoFormat::eNoRole)==OpalVideoFormat::ePresentation,accepted,w,h,rate);}
static std::filesystem::path Bin(){std::wstring s(32768,L'\0');auto n=GetModuleFileNameW(nullptr,s.data(),DWORD(s.size()));s.resize(n);return std::filesystem::path(s).parent_path();}
class StudioH264Encoder : public OpalVideoTranscoder {
 std::unique_ptr<telestudio::IVideoEncoder> encoder;
 int width=0,height=0,bitrate=0,fps=0;bool text=true,singleNal=false;
public:
 explicit StudioH264Encoder(bool mode0=false):OpalVideoTranscoder(OpalYUV420P,mode0?OpalH264_MODE0:OpalH264),singleNal(mode0){}
 PBoolean ConvertFrames(const RTP_DataFrame&input,RTP_DataFrameList&output) override{
  output.RemoveAll();try{
   if(input.GetPayloadSize()<sizeof(FrameHeader))return false;
   auto*h=reinterpret_cast<const FrameHeader*>(input.GetPayloadPtr());
   if(h->x||h->y||h->width<2||h->height<2||h->width>4096||h->height>2160||h->width%2||h->height%2||input.GetPayloadSize()<OpalVideoFrameSizeForResolution(h->width,h->height))return false;
   int requested=std::clamp(outputMediaFormat.GetOptionInteger(OpalMediaFormat::TargetBitRateOption(),2048000)/1000,64,16000);
   int rate=std::clamp(90000/std::max(1,int(outputMediaFormat.GetFrameTime())),1,60);bool content=textContent;telestudio::EncoderSettings settings{telestudio::VideoCodec::H264,int(h->width),int(h->height),rate,requested,false,rate*(content?4:2)};
   if(!encoder){encoder=telestudio::CreateOpalX264Encoder(Bin()/"libx264-165.dll",singleNal?1100:0);encoder->Open(settings);}else if(width!=int(h->width)||height!=int(h->height)||requested!=bitrate||rate!=fps||content!=text)encoder->Reconfigure(settings);
   width=h->width;height=h->height;bitrate=requested;fps=rate;text=content;
   const auto*p=OpalVideoFrameDataPtr(h);auto count=width*height;
   telestudio::I420Frame view{p,p+count,p+count+count/4,width,width/2,width/2,width,height,int64_t(input.GetTimestamp())*10000000/90000};
   auto unit=encoder->Encode(view,m_encodingIntraFrameControl.RequireIntraFrame());
   auto packets=telestudio::PacketizeH264(unit.annexB,std::min<size_t>(1200,GetMaxOutputSize()>12?GetMaxOutputSize()-12:1200));
   if(singleNal)for(auto&packet:packets)if(packet.bytes.empty()||(packet.bytes[0]&31)>=24)throw std::runtime_error("H264 mode0 requires bounded single NAL packets");
   for(auto&packet:packets){auto*f=new RTP_DataFrame(packet.bytes.size());std::memcpy(f->GetPayloadPtr(),packet.bytes.data(),packet.bytes.size());f->SetTimestamp(input.GetTimestamp());f->SetMarker(packet.marker);f->SetPayloadType(outputMediaFormat.GetPayloadType());output.Append(f);}
   m_lastFrameWasIFrame=unit.keyFrame;if(unit.keyFrame)m_encodingIntraFrameControl.IntraFrameDetected();Observe(input,outputMediaFormat,!packets.empty(),width,height,bitrate);return true;
  }catch(const std::exception&e){Observe(input,outputMediaFormat,false,0,0,0);PTRACE(1,"MyStudio H264 encode "<<e.what());return false;}
 }
};
class StudioH264Decoder : public OpalVideoTranscoder {
 telestudio::FfmpegVideoDecoder decoder{{Bin()},telestudio::DecoderPreference::SoftwareOnly};
 bool opened=false,missing=false,started=false,singleNal=false;uint32_t timestamp=0;uint16_t sequence=0;
 size_t bytes=0;std::vector<std::vector<uint8_t>> pending;
public:
 explicit StudioH264Decoder(bool mode0=false):OpalVideoTranscoder(mode0?OpalH264_MODE0:OpalH264,OpalYUV420P),singleNal(mode0){}
 PBoolean ConvertFrames(const RTP_DataFrame&input,RTP_DataFrameList&output) override{
  output.RemoveAll();try{
   if(input.GetPayloadSize()==0)return true;
   if(singleNal&&(input.GetPayloadPtr()[0]&31)>=24){pending.clear();bytes=0;return true;}
   if(started&&timestamp!=input.GetTimestamp()){pending.clear();bytes=0;missing=false;}
   if(started&&timestamp==input.GetTimestamp()&&uint16_t(sequence+1)!=input.GetSequenceNumber())missing=true;
   timestamp=input.GetTimestamp();sequence=input.GetSequenceNumber();started=true;
   bytes+=input.GetPayloadSize();if(bytes>8*1024*1024||pending.size()>8192){pending.clear();bytes=0;missing=true;return true;}
   pending.emplace_back(input.GetPayloadPtr(),input.GetPayloadPtr()+input.GetPayloadSize());if(!input.GetMarker())return true;
   std::vector<telestudio::RtpPayloadView> views;for(auto&p:pending)views.push_back({p,missing});auto unit=telestudio::DepacketizeH264(views);pending.clear();bytes=0;missing=false;
   if(unit.status!=telestudio::RtpDepacketizeStatus::Complete){if(!GetCommandNotifier().IsNULL())SendIFrameRequest(sequence,timestamp);return true;}
   if(!opened){decoder.Open(telestudio::VideoCodec::H264,1280,720);opened=true;}
   auto result=decoder.Decode(unit.annexB,int64_t(timestamp)*10000000/90000);
   if(result.keyframeRequested)if(!GetCommandNotifier().IsNULL())SendIFrameRequest(sequence,timestamp);
   for(auto&bgra:result.frames){auto yuv=telestudio::ConvertBgraToI420(bgra.bytes,bgra.width,bgra.height,bgra.stride,bgra.timestamp100ns);auto*f=new RTP_DataFrame(OpalVideoFrameSizeForResolution(bgra.width,bgra.height));auto*h=reinterpret_cast<FrameHeader*>(f->GetPayloadPtr());*h={0,0,unsigned(bgra.width),unsigned(bgra.height)};auto*p=OpalVideoFrameDataPtr(h);std::memcpy(p,yuv.Y().data(),yuv.YSize());std::memcpy(p+yuv.YSize(),yuv.U().data(),yuv.USize());std::memcpy(p+yuv.YSize()+yuv.USize(),yuv.V().data(),yuv.VSize());f->SetTimestamp(timestamp);f->SetMarker(true);output.Append(f);}
   m_lastFrameWasIFrame=unit.randomAccess;return true;
  }catch(const std::exception&e){PTRACE(1,"MyStudio H264 decode "<<e.what());return false;}
 }
};
class StudioH264Mode0Encoder : public StudioH264Encoder {public:StudioH264Mode0Encoder():StudioH264Encoder(true){}};
class StudioH264Mode0Decoder : public StudioH264Decoder {public:StudioH264Mode0Decoder():StudioH264Decoder(true){}};
void RegisterH264(){
 static OpalTranscoderFactory::Worker<StudioH264Encoder> encoder(MakeOpalTranscoderKey(OpalYUV420P,OpalH264));
 static OpalTranscoderFactory::Worker<StudioH264Decoder> decoder(MakeOpalTranscoderKey(OpalH264,OpalYUV420P));
 static OpalTranscoderFactory::Worker<StudioH264Mode0Encoder> mode0encoder(MakeOpalTranscoderKey(OpalYUV420P,OpalH264_MODE0));
 static OpalTranscoderFactory::Worker<StudioH264Mode0Decoder> mode0decoder(MakeOpalTranscoderKey(OpalH264_MODE0,OpalYUV420P));
}
}
