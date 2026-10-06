#include <telestudio/media/OutboundAudioMixer.h>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace telestudio::media;
namespace {
void Require(bool ok,const char* reason){if(!ok)throw std::runtime_error(reason);}
OwnedAudioBlock Block(AudioTapKind source,int16_t value,int slot=0){return {std::vector<int16_t>(960,value),48000,1,int64_t(slot)*200000,200000,source,42,false};}
void Configure(OutboundAudioMixer& mixer){Require(mixer.SetVoicePriorityMix(25),"voice-priority levels rejected");}
void CheckLevels(int16_t mic,int16_t system,int16_t expected,bool micMuted=false){
 OutboundAudioMixer mixer;Configure(mixer);mixer.SetMicrophoneMuted(micMuted);
 mixer.PushMicrophone(Block(AudioTapKind::Microphone,mic));mixer.PushSystemAudio(Block(AudioTapKind::SystemAudio,system));
 auto result=mixer.TakeMixedBlock();
 Require(std::all_of(result.samples.begin(),result.samples.end(),[&](auto value){return value==expected;}),"voice reduced or shared system audio unbalanced");
 Require(result.duration100ns==200000&&result.callGeneration==42,"mixed block timing/generation changed");
}
}
int main(){try{
 CheckLevels(4000,0,4000); // Silent loopback must not halve the microphone.
 CheckLevels(4000,12000,7000); // Full mic plus 25% system audio.
 CheckLevels(4000,12000,3000,true); // Mic mute leaves the attenuated system audible.
 CheckLevels(32767,32767,32767); // Limit the whole block before narrowing.
 CheckLevels(-32768,-32768,-32767);
 OutboundAudioMixer solo;Configure(solo);solo.PushMicrophone(Block(AudioTapKind::Microphone,4000));
 Require(solo.TakeMixedBlock().samples[0]==4000,"mic-only voice changed");
 OutboundAudioMixer system;Configure(system);system.PushSystemAudio(Block(AudioTapKind::SystemAudio,12000));
 Require(system.TakeMixedBlock().samples[0]==3000,"system-only path bypassed level control");
 std::cout<<"voice_priority_mix: PASS\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
