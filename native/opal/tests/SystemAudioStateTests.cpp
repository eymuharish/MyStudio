#include "../OpalMedia.h"
#include <iostream>
#include <stdexcept>
#include <vector>
#include <algorithm>
#include <thread>
using namespace telestudio::media;
struct Backend final : IProcessLoopbackBackend {
 ActivationCallback activation;
 PacketCallback packets;
 int starts=0,stops=0;
 bool Start(uint32_t,ProcessLoopbackMode mode,ActivationCallback a,PacketCallback p,std::string&)override{
  if(mode!=ProcessLoopbackMode::ExcludeTargetProcessTree)throw std::runtime_error("Loopback exclusion changed");
  ++starts;activation=std::move(a);packets=std::move(p);return true;
 }
 bool Stop(uint32_t)noexcept override{++stops;return true;}
 void Ready(){activation(true,{48000,2,LoopbackSampleFormat::Pcm16},{});}
 void Fail(){activation(false,{},"IAudioCaptureClient failed (0x88890004)");}
};
void Require(bool value,const char*message){if(!value)throw std::runtime_error(message);}
int main(){try{
 _putenv_s("ESSASTUDIO_TEST_MODE","1");Backend backend;studio::opal::StudioMedia media({}, {}, &backend);media.Start(100);
 std::vector<std::pair<std::string,std::string>> events;
 auto pump=[&]{media.Pump([&](auto type,auto fields){if(type=="system-audio-changed"||type=="media-error")events.emplace_back(type,fields);},false);};
 auto confirmed=[&](bool enabled){return std::any_of(events.begin(),events.end(),[&](const auto&event){return event.first=="system-audio-changed"&&event.second==(enabled?"\"enabled\":true":"\"enabled\":false");});};
 Require(media.SystemAudio(true),"Initial activation rejected");pump();
 Require(events.empty(),"Pending activation was confirmed off");
 Require(media.SystemAudio(true)&&backend.starts==1,"Duplicate ON restarted pending capture");
 backend.Ready();pump();Require(!events.empty()&&events.back().second=="\"enabled\":true","Async ready never confirmed sharing");
 events.clear();Require(media.SystemAudio(true)&&backend.starts==1,"Duplicate ON restarted running capture");pump();
 Require(events.size()==1&&events.back().second=="\"enabled\":true","Idempotent ON was not acknowledged");
 events.clear();media.SystemAudio(false);pump();Require(!media.systemAudio.IsRunning()&&events.back().second=="\"enabled\":false","OFF did not confirm stopped capture");
 media.SystemAudio(true);auto stale=backend.activation;media.SystemAudio(false);stale(true,{48000,2,LoopbackSampleFormat::Pcm16},{});pump();
 Require(!media.systemAudio.IsRunning(),"Late activation revived OFF capture");
 events.clear();media.SystemAudio(true);backend.Fail();pump();
 Require(!media.systemAudio.IsRunning()&&confirmed(false),"Invalidation did not confirm stopped sharing");
 Require(std::any_of(events.begin(),events.end(),[](const auto&event){return event.first=="media-error"&&event.second.find("0x88890004")!=std::string::npos;}),"Backend failure detail was not reported");
 int beforeRetry=backend.starts;std::this_thread::sleep_for(std::chrono::milliseconds(550));pump();
 Require(backend.starts==beforeRetry+1,"Transient invalidation did not automatically reactivate capture");
 backend.Ready();events.clear();pump();Require(confirmed(true),"Automatic recovery was not confirmed");
 backend.Fail();pump();media.SystemAudio(false);beforeRetry=backend.starts;std::this_thread::sleep_for(std::chrono::milliseconds(550));pump();
 Require(backend.starts==beforeRetry&&!media.systemAudio.IsRunning(),"OFF did not cancel pending recovery");
 Require(media.SystemAudio(true),"Failed activation could not be retried with one click");backend.Ready();pump();
 Require(media.systemAudio.IsRunning()&&events.back().second=="\"enabled\":true","Recovery was not confirmed");
 beforeRetry=backend.starts;
 for(int attempt=0;attempt<4;++attempt){backend.Fail();pump();std::this_thread::sleep_for(std::chrono::milliseconds(550));pump();}
 Require(backend.starts==beforeRetry+3,"Device invalidation retries were not bounded to three");
 media.SystemAudio(false);
 media.SystemAudio(true);backend.activation(false,{},"process-loopback activation timed out after five seconds");events.clear();pump();
 Require(confirmed(false)&&!media.systemAudio.IsStarting(),"Activation timeout did not finish the pending state");
 media.Stop();std::cout<<"OPAL system audio async state PASS\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
