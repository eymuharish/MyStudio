#include "../OpalSharedRecordingWorker.h"
#include <Windows.h>
#include <array>
#include <cmath>
#include <iostream>
#include <thread>
#include <stdexcept>
using namespace telestudio;
int main(int argc,char** argv) { try {
 const auto directory=argc>1?std::filesystem::path(argv[1]):std::filesystem::temp_directory_path()/("MyStudio-voice-proof-"+std::to_string(GetCurrentProcessId()));
 std::filesystem::create_directories(directory);
 wchar_t executable[32768];GetModuleFileNameW(nullptr,executable,32768);
 auto worker=CreateOpalSharedRecordingWorker(std::filesystem::path(executable).parent_path()/L"libx264-165.dll",directory);
 constexpr int64_t epoch=1000000000; constexpr uint64_t generation=27;
 if(worker->Start({"owned-selected-source",true,true},generation,epoch)!=RecordingError::None)throw std::runtime_error("Voice fixture start failed");
 std::array<uint8_t,256> y; std::array<uint8_t,64> u,v; y.fill(200);u.fill(128);v.fill(128);
 I420Frame input{y.data(),u.data(),v.data(),16,8,8,16,16,epoch};
 auto pixels=std::make_shared<ComposedVideoFrame>(ComposeVideoFrame(&input,nullptr,{1920,1080},epoch));
 worker->PushVideo(pixels,epoch);
 auto poll=[&](RecordingWorkerUpdateKind wanted) {
  for(int retry=0;retry<500;++retry) {
   if(auto update=worker->Poll()) { if(update->kind==wanted)return *update; if(update->kind==RecordingWorkerUpdateKind::Failed)throw std::runtime_error("Actual voice recording failed"); }
   std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  throw std::runtime_error("Voice recording timed out");
 };
 poll(RecordingWorkerUpdateKind::Ready);
 for(int frame=0;frame<200;++frame) {
  for(auto source:{media::AudioTapKind::Microphone,media::AudioTapKind::Room}) {
   media::OwnedAudioBlock block;block.source=source;block.sampleRateHz=48000;block.channels=1;
   block.timestamp100ns=epoch+frame*200000LL;block.duration100ns=200000;block.callGeneration=generation;block.samples.resize(960);
   const double frequency=source==media::AudioTapKind::Microphone?1000:700, amplitude=source==media::AudioTapKind::Microphone?500:5000;
   for(size_t n=0;n<960;++n)block.samples[n]=static_cast<int16_t>(amplitude*std::sin(2*3.141592653589793*frequency*(frame*960+n)/48000));
   if(!worker->Push(std::move(block)))throw std::runtime_error("Valid voice fixture PCM rejected");
  }
  if(frame>0 && frame%2==0)worker->PushVideo(pixels,epoch+frame*200000LL);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
 }
 worker->Stop();
 if(worker->WaitQuiesced(std::chrono::seconds(5))!=RecordingWaitOutcome::Quiesced)throw std::runtime_error("Voice recording did not finalize");
 const auto complete=poll(RecordingWorkerUpdateKind::Completed);
 if(complete.fileName.empty())throw std::runtime_error("Completed voice recording has no file");
 std::cout<<"VOICE_MP4="<<(directory/complete.fileName).string()<<'\n';return 0;
 }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
