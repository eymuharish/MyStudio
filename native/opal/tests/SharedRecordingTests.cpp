#include "../OpalMedia.h"
#include <windows.h>
#include <atomic>
#include <algorithm>
#include <iostream>
#include <thread>
#include <stdexcept>
using namespace telestudio;
void RecordingDiagnosticFailureProof(const std::filesystem::path&);
void OpalLateAudioProof(const std::filesystem::path&);
std::atomic<unsigned> captures{0},recorded{0};
class SelectedCapture:public IDesktopCaptureSource {
public:
 void Start(std::string_view id,int,FrameCallback frame,CaptureErrorCallback)override{++captures;std::array<uint8_t,256>y;std::array<uint8_t,64>u,v;y.fill(id=="window:chosen"?200:16);u.fill(128);v.fill(128);frame({y.data(),u.data(),v.data(),16,8,8,16,16,1});}
 void Stop()override{}
};
void Require(bool value,const char*message){if(!value)throw std::runtime_error(message);}
void DelayedRoomRecordingProof(const std::filesystem::path& directory){
 _putenv_s("ESSASTUDIO_TEST_RECORDING_OUTPUT_DIR",directory.string().c_str());
 studio::opal::StudioMedia media([]{return std::make_shared<SelectedCapture>();});media.Start(71);media.Desktop(true,"window:chosen");
 uint32_t stamp=9000;auto frame=[&]{std::this_thread::sleep_for(std::chrono::milliseconds(35));media.Pump([](auto,auto){});std::vector<uint8_t> pixels;Require(media.ReadVideo(pixels,320,240,stamp),"Delayed room fixture lost output");media.VideoAccepted(stamp,false,true,320,240,4000);stamp+=3000;media.Pump([](auto,auto){});};
 for(int i=0;i<4;++i)frame();Require(media.playbackSamples==0,"Delayed room fixture unexpectedly had PCM");
 Require(media.StartRecording({"window:chosen",false,true}).error==RecordingError::None,"Room PCM arriving after connected must defer recording rather than reject it");
 for(int i=0;i<8;++i)frame();Require(media.recorder->Snapshot().state!=RecordingState::Recording,"Recorder started before requested room PCM existed");
 std::array<int16_t,960> pcm;pcm.fill(2400);size_t written=0;
 for(int i=0;i<24;++i){Require(media.WriteAudio(pcm.data(),sizeof(pcm),written,48000,1)&&written==sizeof(pcm),"Delayed room PCM was rejected");frame();}
 Require(media.recorder->Snapshot().state==RecordingState::Recording&&media.recorder->Snapshot().roomIncluded,"Ready room PCM did not start the real encoder with room audio");
 media.Stop();for(int i=0;i<120&&media.recorder->Snapshot().state==RecordingState::Finalizing;++i){media.Pump([](auto,auto){});std::this_thread::sleep_for(std::chrono::milliseconds(30));}
 auto result=media.recorder->Snapshot();Require(result.state==RecordingState::Completed&&std::filesystem::file_size(directory/result.fileName)>0,"Delayed room recording did not finalize a real MP4");
 std::cout<<"Delayed room PCM actual recording PASS\n";
}
void PendingRecordingCancellationProof(const std::filesystem::path& directory){
 for(int scenario=0;scenario<5;++scenario){
  auto output=directory/("pending-cancel-"+std::to_string(scenario));std::filesystem::create_directory(output);_putenv_s("ESSASTUDIO_TEST_RECORDING_OUTPUT_DIR",output.string().c_str());
  studio::opal::StudioMedia media([]{return std::make_shared<SelectedCapture>();});media.Start(81);media.Desktop(true,"window:chosen");
  std::vector<std::pair<std::string,std::string>> events;auto collect=[&](auto type,auto fields){events.emplace_back(type,fields);};uint32_t stamp=9000;
  auto frame=[&]{std::this_thread::sleep_for(std::chrono::milliseconds(35));media.Pump(collect);std::vector<uint8_t> pixels;Require(media.ReadVideo(pixels,320,240,stamp),"Pending cancellation fixture lost output");media.VideoAccepted(stamp,false,true,320,240,4000);stamp+=3000;media.Pump(collect);};
  for(int i=0;i<4;++i)frame();Require(media.StartRecording({"window:chosen",false,true}).error==RecordingError::None,"Pending cancellation fixture rejected initial request");
  Require(media.StartRecording({"window:chosen",false,false}).error==RecordingError::AlreadyRecording,"Duplicate start replaced the pending room-audio request");
  if(scenario!=4)media.Pump(collect);Require(media.RecordingStatus().state==RecordingState::Starting&&media.RecordingStatus().roomIncluded,"Pending snapshot lost requested room audio");
  if(scenario==0)media.StopRecording();else if(scenario==1)media.Desktop(true,"monitor:whole-PC");else if(scenario==2){media.Stop();media.Start(82);media.Desktop(true,"window:chosen");}else if(scenario==4){media.Stop();media.Start(82);media.Desktop(true,"window:chosen");}else{
   auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(3400);while(std::chrono::steady_clock::now()<deadline)frame();
  }
  media.Pump(collect);Require(media.RecordingStatus().state==RecordingState::Failed,"Cancelled/expired pending recording stranded Starting");
  if(scenario==4)for(const auto& event:events)Require(event.first!="recording-state"||event.second.find("\"state\":\"starting\"")==std::string::npos,"Call teardown emitted queued Starting after terminal");
  auto failure=std::find_if(events.begin(),events.end(),[](const auto& e){return e.first=="recording-error";});
  Require(failure!=events.end()&&failure->second.find(scenario==3?"recording-start-timeout":"recording-start-cancelled")!=std::string::npos,"Cancellation/timeout lost its fatal diagnostic");
  Require(std::next(failure)!=events.end()&&std::next(failure)->first=="recording-state"&&std::next(failure)->second.find("\"state\":\"failed\"")!=std::string::npos,"Failed must immediately follow fatal recording error");
  std::array<int16_t,960> pcm;pcm.fill(2400);size_t written=0;for(int i=0;i<8;++i){Require(media.WriteAudio(pcm.data(),sizeof(pcm),written,48000,1),"Late cancellation PCM rejected");frame();}
  Require(media.recorder->Snapshot().state!=RecordingState::Recording,"Late PCM started an abandoned recording");for(const auto& item:std::filesystem::directory_iterator(output))Require(item.path().filename()==".essastudio-recording.lock","Abandoned recording opened output files");media.Stop();
 }
 std::cout<<"Pending recording duplicate/Stop/source/call/timeout isolation PASS\n";
}
int main(){try{_putenv_s("ESSASTUDIO_TEST_MODE","1");char temp[MAX_PATH];GetTempPathA(MAX_PATH,temp);auto directory=std::filesystem::path(temp)/("MyStudio-opal-shared-test-"+std::to_string(GetCurrentProcessId()));std::filesystem::create_directories(directory);RecordingDiagnosticFailureProof(directory);OpalLateAudioProof(directory);DelayedRoomRecordingProof(directory);PendingRecordingCancellationProof(directory);captures=recorded=0;_putenv_s("ESSASTUDIO_TEST_RECORDING_OUTPUT_DIR",directory.string().c_str());studio::opal::StudioMedia media([]{return std::make_shared<SelectedCapture>();},[](I420Frame frame){Require(frame.width==1920&&frame.height==1080,"Recording input must be Full HD");Require(frame.y[frame.strideY*(frame.height/2)+frame.width/2]==200,"Recorder received the whole monitor or compositor instead of the chosen window");++recorded;});media.Start(1);
 Require(media.StartRecording({"window:chosen",false,false}).error==RecordingError::InvalidTarget&&captures==0,"Inactive recording started its own capture");
 media.Desktop(true,"window:chosen");Require(media.StartRecording({"window:chosen",false,false}).error==RecordingError::InvalidTarget,"Unacknowledged share accepted recording");
 uint32_t timestamp=9000;auto frame=[&]{std::this_thread::sleep_for(std::chrono::milliseconds(35));media.Pump([](auto,auto){});std::vector<uint8_t>bytes;Require(media.ReadVideo(bytes,320,240,timestamp),"Shared output frame missing");media.VideoAccepted(timestamp,false,true,320,240,4000);timestamp+=3000;media.Pump([](auto,auto){});};
 for(int i=0;i<4;++i)frame();Require(media.SharedRecordingSource("window:chosen"),"Actual output acknowledgment did not confirm sharing");Require(media.StartRecording({"monitor:whole-PC",false,false}).error==RecordingError::InvalidTarget,"Different source accepted recording");Require(media.StartRecording({"window:chosen",false,false}).error==RecordingError::None,"Confirmed chosen source rejected recording");
 for(int i=0;i<20;++i)frame();Require(media.recorder->Snapshot().state==RecordingState::Recording&&recorded>5,"Push-fed recording never encoded chosen source pixels");Require(media.recorder->Pause().error==RecordingError::None,"Shared recording pause failed");auto paused=recorded.load();for(int i=0;i<5;++i)frame();Require(recorded==paused,"Paused recording encoded additional pixels");Require(media.ResumeRecording().error==RecordingError::None,"Shared recording resume failed");for(int i=0;i<15;++i)frame();
 Require(captures==1,"Recording opened another capture source");
 media.Desktop(false);media.Pump([](auto,auto){});Require(media.recorder->Snapshot().state==RecordingState::Recording&&media.recorder->Snapshot().paused,"Stopping share must pause the existing MP4");auto stopped=recorded.load();for(int i=0;i<5;++i)frame();Require(recorded==stopped,"Stopped share leaked pixels into paused recording");Require(media.ResumeRecording().error==RecordingError::InvalidTarget,"Inactive sharing accepted Resume");
 media.Desktop(true,"window:chosen");Require(media.ResumeRecording().error==RecordingError::InvalidTarget,"Unacknowledged sharing accepted Resume");for(int i=0;i<4;++i)frame();Require(media.recorder->Snapshot().paused,"Restarting sharing resumed recording automatically");Require(media.ResumeRecording().error==RecordingError::None,"Confirmed chosen source could not explicitly Resume");for(int i=0;i<12;++i)frame();Require(recorded>stopped,"Explicit Resume did not encode confirmed selected pixels");stopped=recorded.load();
 media.Desktop(true,"monitor:whole-PC");stopped=recorded.load();for(int i=0;i<5;++i)frame();Require(media.recorder->Snapshot().paused,"Changing to another source must keep recording paused");Require(recorded==stopped,"Sharing another source resumed recording automatically");media.recorder->Stop();for(int i=0;i<100&&media.recorder->Snapshot().state==RecordingState::Finalizing;++i){media.Pump([](auto,auto){});std::this_thread::sleep_for(std::chrono::milliseconds(30));}auto result=media.recorder->Snapshot();Require(result.state==RecordingState::Completed,"Explicit Stop did not finalize the original recording");media.Desktop(false);Require(!media.SharedRecordingSource("window:chosen")&&media.StartRecording({"window:chosen",false,false}).error==RecordingError::InvalidTarget,"Stopped sharing accepted recording");Require(captures==3,"Capture starts must match only the three explicit share choices");media.Desktop(true,"window:chosen");for(int i=0;i<4;++i)frame();Require(media.StartRecording({"window:chosen",false,false}).error==RecordingError::None,"Second confirmed recording rejected");for(int i=0;i<15;++i)frame();media.Desktop(false);media.Stop();bool finalized=false;for(int i=0;i<100&&!finalized;++i){media.Pump([&](std::string_view type,const std::string&fields){if(type=="recording-state"){Require(fields.find("\"state\":\"recording\"")==std::string::npos&&fields.find("\"state\":\"starting\"")==std::string::npos,"Teardown emitted stale recording/paused state after call clear");if(fields.find("\"state\":\"completed\"")!=std::string::npos)finalized=true;}});std::this_thread::sleep_for(std::chrono::milliseconds(30));}Require(finalized,"Paused call teardown did not complete MP4");std::cout<<"Shared chosen-window recording privacy PASS frames="<<recorded<<" captures="<<captures<<" MP4="<<(directory/result.fileName).string()<<'\n';return 0;}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
