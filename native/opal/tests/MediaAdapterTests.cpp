#include "../OpalMedia.h"
#include <codec/opalplugin.h>
#include <windows.h>
#include <iostream>
#include <cmath>
#include <thread>
int main(){try{
 _putenv_s("ESSASTUDIO_TEST_MODE","1");studio::opal::StudioMedia media;media.sendBufferMs=150;media.Start(1);size_t silenceBytes=99;if(!media.WriteAudio(nullptr,0,silenceBytes,16000,1)||silenceBytes!=0||media.playbackSamples!=0)throw std::runtime_error("Initial jitter silence must preserve the receive patch without claiming decoded samples");
 {
  studio::opal::StudioMedia restarted; std::array<int16_t,320>partial{}; partial.fill(12000); size_t written=0;
  restarted.Start(3); restarted.WriteAudio(partial.data(),100*2,written,16000,1);
  restarted.Stop(); restarted.Start(3); restarted.WriteAudio(partial.data(),220*2,written,16000,1);
  if(restarted.deviceSubmittedNonzeroSamples!=0)throw std::runtime_error("Restarted media reused a previous-call partial playback block");
  restarted.WriteAudio(partial.data(),100*2,written,16000,1);
  if(restarted.deviceSubmittedNonzeroSamples==0)throw std::runtime_error("Fresh complete playback block was never submitted");
 }
 for(unsigned i=0;i<20;++i){std::array<int16_t,320>pcm{};size_t bytes=0;
  if(!media.ReadAudio(pcm.data(),pcm.size()*2,bytes,16000,1)||bytes!=pcm.size()*2)throw std::runtime_error("Capture callback failed");
  if(!media.WriteAudio(pcm.data(),bytes,bytes,16000,1))throw std::runtime_error("Receive callback failed");
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
 }
 if(media.captureSamples<1000||media.decodedNonzeroSamples<1000)throw std::runtime_error("No actual capture/decoded PCM");
 if(media.deviceSubmittedNonzeroSamples<1000)throw std::runtime_error("Decoded room audio never reaches Windows speaker");
 std::vector<uint8_t>video;if(!media.ReadVideo(video,320,240,9000)||!media.WriteVideo(video.data(),video.size()))throw std::runtime_error("Test card pipeline failed");
 auto*head=reinterpret_cast<PluginCodec_Video_FrameHeader*>(video.data());head->width=4097;
 if(media.WriteVideo(video.data(),video.size()))throw std::runtime_error("Invalid decoded picture accepted");
 media.Stop();size_t length=0;std::array<int16_t,320>pcm{};
 if(media.ReadAudio(pcm.data(),pcm.size()*2,length,16000,1))throw std::runtime_error("Old call audio accepted after stop");
 std::cout<<"OPAL Windows media adapter PASS decoded="<<media.decodedNonzeroSamples<<" deviceSubmitted="<<media.deviceSubmittedNonzeroSamples<<'\n';return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
