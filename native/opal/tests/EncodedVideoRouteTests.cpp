#include <ptlib.h>
#include <ptlib/pprocess.h>
#include <codec/vidcodec.h>
#include <opal/transcoders.h>
#include "../OpalH264.h"
#include "../OpalMedia.h"
#include <array>
#include <cstring>
#include <deque>
#include <iostream>
#include <thread>

class TestProcess : public PProcess {
 PCLASSINFO(TestProcess,PProcess);
public:TestProcess():PProcess("MyStudio","EncodedVideoRouteTests",1,0,ReleaseCode,0){}void Main()override{}
};
static void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
class WhiteWindow final : public telestudio::IDesktopCaptureSource {
 void Start(std::string_view,int,telestudio::FrameCallback callback,telestudio::CaptureErrorCallback)override{
  std::array<uint8_t,256> y;std::array<uint8_t,64> u,v;y.fill(200);u.fill(128);v.fill(128);
  callback({y.data(),u.data(),v.data(),16,8,8,16,16,1});
 }
 void Stop()override{}
};
struct Stream {
 std::unique_ptr<OpalTranscoder> encoder,decoder;
 WORD sequence=1;unsigned packets=0,decodedFrames=0,submittedFrames=0;
 std::deque<std::pair<bool,bool>> expectedPixels;
 bool presentation;
 explicit Stream(bool content):presentation(content){
  OpalMediaFormat format=OpalH264;
  format.SetOptionEnum(OpalVideoFormat::ContentRoleOption(),content?OpalVideoFormat::ePresentation:OpalVideoFormat::eMainRole);
  format.SetOptionInteger(OpalMediaFormat::TargetBitRateOption(),640000);
  encoder.reset(OpalTranscoder::Create(OpalYUV420P,format));
  decoder.reset(OpalTranscoder::Create(format,OpalYUV420P));
  Require(encoder&&decoder,"Independent H264 stream could not be created");
 }
 void Roundtrip(studio::opal::StudioMedia& media,uint32_t timestamp,bool white=false,bool black=false){
  const auto before=decodedFrames;
  ++submittedFrames;
  expectedPixels.emplace_back(white,black);
  const int width=presentation?640:320,height=presentation?360:240;
  std::vector<uint8_t> bytes;Require(media.ReadVideo(bytes,width,height,timestamp,presentation),"Video stream read failed");
  const auto* rawHeader=reinterpret_cast<const PluginCodec_Video_FrameHeader*>(bytes.data());
  const auto rawY=OpalVideoFrameDataPtr(rawHeader)[(height/2)*width+width/2];
  if(black&&rawY>=25)std::cout<<"raw stopped presentation stamp="<<timestamp<<" y="<<unsigned(rawY)<<'\n';
  Require(!black||rawY<25,"Stopped presentation retained selected-window pixels before encoding");
  RTP_DataFrame raw(bytes.size());std::memcpy(raw.GetPayloadPtr(),bytes.data(),bytes.size());raw.SetTimestamp(timestamp);
  RTP_DataFrameList encoded,decoded;Require(encoder->ConvertFrames(raw,encoded)&&!encoded.IsEmpty(),"H264 stream produced no packets");
  packets+=encoded.GetSize();
  for(auto& packet:encoded){
   packet.SetSequenceNumber(sequence++);packet.SetSyncSource(presentation?222:111);
   Require(decoder->ConvertFrames(packet,decoded),"Independent H264 decoder rejected its packet");
   for(auto& frame:decoded){
    Require(frame.GetTimestamp()==timestamp,"A decoded stream used the other stream's timestamp");
    const auto* header=reinterpret_cast<const PluginCodec_Video_FrameHeader*>(frame.GetPayloadPtr());
    Require(header->width==unsigned(width)&&header->height==unsigned(height),"Video stream dimensions crossed");
    const auto* pixels=OpalVideoFrameDataPtr(header);const auto count=width*height;
    const auto center=(height/2)*width+width/2,chroma=(height/4)*(width/2)+width/4;
    const auto y=pixels[center],u=pixels[count+chroma],v=pixels[count+count/4+chroma];
    Require(!expectedPixels.empty(),"Decoder returned a frame without an input in this session");
    const auto expected=expectedPixels.front();expectedPixels.pop_front();
    if(decodedFrames<2||expected.second||expected.first!=presentation)std::cout<<(presentation?"presentation":"main")<<" stamp="<<timestamp<<" y="<<unsigned(y)<<" u="<<unsigned(u)<<" v="<<unsigned(v)<<" expected="<<(expected.second?"black":expected.first?"white":"main")<<'\n';
    if(expected.second)Require(y<25&&u>=120&&u<=136&&v>=120&&v<=136,"Stopped presentation retained selected-window pixels");
    else if(expected.first)Require(y>=190&&y<=210&&u>=120&&u<=136&&v>=120&&v<=136,"Encoded share switched away from the selected white window");
    else Require(int(v)-int(u)>30,"Encoded main video was replaced by presentation pixels");
    ++decodedFrames;
   }
  }
  // The FFmpeg parser retains the first access unit until the next frame boundary.
  // Thereafter every submitted frame must release exactly one prior frame, whose
  // expected pixels stay in this session's FIFO across all routing transitions.
  if(timestamp==9000)Require(decodedFrames==0&&expectedPixels.size()==1,"Unexpected decoder startup delay");
  else Require(decodedFrames==before+1&&expectedPixels.size()==1,"A routing transition failed to sustain the one-frame decode pipeline");
 }
};
int main(){
 _putenv_s("PTLIBPLUGINDIR","MyStudio-test-no-auto-plugins");_putenv_s("ESSASTUDIO_TEST_MODE","1");TestProcess process;
 try{
  const auto recordingDirectory=std::filesystem::temp_directory_path()/("MyStudio-mirrored-recording-test-"+std::to_string(GetCurrentProcessId()));
  std::filesystem::create_directories(recordingDirectory);
  _putenv_s("ESSASTUDIO_TEST_RECORDING_OUTPUT_DIR",recordingDirectory.string().c_str());
  studio::opal::RegisterH264();studio::opal::StudioMedia media([]{return std::make_shared<WhiteWindow>();});media.Start(1);
  media.Desktop(true,"window:white",true);std::this_thread::sleep_for(std::chrono::milliseconds(200));
  std::vector<uint8_t> proof;
  Require(media.ReadVideo(proof,320,240,8100,false),"Mirrored main recording proof read failed");
  media.VideoAccepted(8100,false,true,320,240,640);
  Require(!media.SharedRecordingSource("window:white"),"Mirrored main acceptance must not feed dedicated-mode recording");
  Require(media.ReadVideo(proof,640,360,8200,true),"Dedicated recording proof read failed");
  media.VideoAccepted(8200,true,true,640,360,640);
  Require(media.SharedRecordingSource("window:white"),"Dedicated presentation acceptance must confirm the one recording lane");
  unsigned mainAccepted=0,presentationAccepted=0;
  studio::opal::SetVideoAcceptance([&](uint32_t stamp,bool content,bool accepted,int width,int height,int rate){
   Require(accepted,"H264 frame was not accepted");++(content?presentationAccepted:mainAccepted);media.VideoAccepted(stamp,content,accepted,width,height,rate);
  });
  Stream main(false),presentation(true);
  for(unsigned frame=0;frame<16;++frame){
   // Deliberately share the RTP timestamp across sessions; sequence and codec state stay independent.
   const auto stamp=9000+frame*3000;
   if(frame%2){presentation.Roundtrip(media,stamp,true);main.Roundtrip(media,stamp,true);}
   else{main.Roundtrip(media,stamp,true);presentation.Roundtrip(media,stamp,true);}
  }
  uint32_t stamp=60000;
  for(unsigned cycle=0;cycle<3;++cycle){
   media.Desktop(false,{},true);
   main.Roundtrip(media,stamp+=3000);presentation.Roundtrip(media,stamp,false,true);
   media.Desktop(true,"window:white",false);std::this_thread::sleep_for(std::chrono::milliseconds(200));
   for(unsigned frame=0;frame<4;++frame){main.Roundtrip(media,stamp+=3000,true);presentation.Roundtrip(media,stamp,false,true);}
   media.Desktop(false,{},false);
   main.Roundtrip(media,stamp+=3000);presentation.Roundtrip(media,stamp,false,true);
   media.Desktop(true,"window:white",true);std::this_thread::sleep_for(std::chrono::milliseconds(200));
   for(unsigned frame=0;frame<4;++frame){presentation.Roundtrip(media,stamp+=3000,true);main.Roundtrip(media,stamp,true);}
  }
  // BFCP grant/loss can switch modes without a user Stop. The same selected
  // source must return on main immediately after capture settles in each mode.
  media.Desktop(true,"window:white",false);std::this_thread::sleep_for(std::chrono::milliseconds(200));
  main.Roundtrip(media,stamp+=3000,true);presentation.Roundtrip(media,stamp,false,true);
  media.Pump([](auto,auto){});
  Require(media.StartRecording({"window:white",false,false}).error==telestudio::RecordingError::None,"Confirmed primary sharing rejected recording");
  for(unsigned attempt=0;attempt<100&&media.recorder->Snapshot().state!=telestudio::RecordingState::Recording;++attempt){
   std::this_thread::sleep_for(std::chrono::milliseconds(20));
   main.Roundtrip(media,stamp+=3000,true);presentation.Roundtrip(media,stamp,false,true);media.Pump([](auto,auto){});
  }
  Require(media.recorder->Snapshot().state==telestudio::RecordingState::Recording&&!media.recorder->Snapshot().paused,"Primary recording never became active");
  media.Desktop(true,"window:white",true);
  Require(!media.recorder->Snapshot().paused,"Same-source BFCP grant automatically paused the active recording");
  for(unsigned attempt=0;attempt<10;++attempt){std::this_thread::sleep_for(std::chrono::milliseconds(20));media.Pump([](auto,auto){});Require(!media.recorder->Snapshot().paused,"Same-source mode transition paused recording while awaiting fresh acceptance");}
  main.Roundtrip(media,stamp+=3000,true);presentation.Roundtrip(media,stamp,true);
  media.Pump([](auto,auto){});Require(!media.recorder->Snapshot().paused,"Dedicated acceptance failed to preserve active recording");
  Require(media.recorder->Pause().error==telestudio::RecordingError::None,"Explicit recording pause failed");
  media.Desktop(true,"window:white",false);std::this_thread::sleep_for(std::chrono::milliseconds(200));
  main.Roundtrip(media,stamp+=3000,true);presentation.Roundtrip(media,stamp,false,true);media.Pump([](auto,auto){});
  Require(media.recorder->Snapshot().paused,"Same-source mode transition automatically resumed a user-paused recording");
  Require(media.ResumeRecording().error==telestudio::RecordingError::None,"Confirmed main output rejected explicit Resume");
  Require(!media.recorder->Snapshot().paused,"Explicit Resume did not resume recording");
  media.Desktop(false,{},false);media.Pump([](auto,auto){});
  Require(media.recorder->Snapshot().paused,"Stopping both shares did not pause recording");
  studio::opal::SetVideoAcceptance({});
  Require(main.decodedFrames+1==main.submittedFrames&&presentation.decodedFrames+1==presentation.submittedFrames,"One video stream failed to sustain decoded frames across stop/restart");
  Require(mainAccepted==main.submittedFrames&&presentationAccepted==presentation.submittedFrames,"Encoder acceptance confused main and presentation roles across stop/restart");
  media.Stop();
  for(unsigned attempt=0;attempt<100&&media.recorder->Snapshot().state!=telestudio::RecordingState::Completed;++attempt){media.Pump([](auto,auto){});std::this_thread::sleep_for(std::chrono::milliseconds(20));}
  const auto recording=media.recorder->Snapshot();
  Require(recording.state==telestudio::RecordingState::Completed,"Mirrored-source recording failed to finalize its MP4");
  Require(std::filesystem::is_regular_file(recordingDirectory/recording.fileName)&&std::filesystem::file_size(recordingDirectory/recording.fileName)>0,"Mirrored-source recording did not write its MP4");
  std::cout<<"encoded video stream routing PASS mainFrames="<<main.decodedFrames<<" mainPackets="<<main.packets<<" presentationFrames="<<presentation.decodedFrames<<" presentationPackets="<<presentation.packets<<'\n';
  return 0;
 }catch(const std::exception& error){studio::opal::SetVideoAcceptance({});std::cerr<<error.what()<<'\n';return 1;}
}
