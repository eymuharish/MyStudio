#include "../OpalRecordingAudioBalance.h"
#include <cmath>
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace telestudio::media;
using studio::opal::RecordingAudioBalance;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
OwnedAudioBlock Tone(AudioTapKind source, int amplitude, int frame) {
 OwnedAudioBlock block; block.source=source; block.sampleRateHz=48000; block.channels=1;
 block.duration100ns=200000; block.timestamp100ns=frame*200000LL; block.callGeneration=1;
 block.samples.resize(960);
 for(size_t n=0;n<block.samples.size();++n) block.samples[n]=static_cast<int16_t>(amplitude*std::sin(n*2*3.141592653589793/48));
 return block;
}
double Rms(const OwnedAudioBlock& block) { double sum=0; for(auto sample:block.samples)sum+=double(sample)*sample; return std::sqrt(sum/block.samples.size()); }
int main() { try {
 RecordingAudioBalance balance(true,true);
 for(int frame=0;frame<80;++frame) {
  auto mic=Tone(AudioTapKind::Microphone,500,frame), room=Tone(AudioTapKind::Room,5000,frame);
  balance.Process(mic); balance.Process(room);
  if(frame==79) { Require(Rms(mic)>2600,"Quiet lecturer microphone was not raised"); Require(Rms(room)<2100,"Room audio still overwhelms lecturer speech"); }
 }
 auto quiet=Tone(AudioTapKind::Microphone,70,80); const auto noise=quiet.samples; balance.Process(quiet);
 Require(quiet.samples==noise,"Background noise was amplified");
 auto mute=Tone(AudioTapKind::Microphone,0,81); balance.Process(mute);
 Require(Rms(mute)==0,"Muted microphone became audible");
 auto loud=Tone(AudioTapKind::Microphone,32000,82); balance.Process(loud);
 Require(*std::max_element(loud.samples.begin(),loud.samples.end())<=27000 && *std::min_element(loud.samples.begin(),loud.samples.end())>=-27000,"Hot microphone clipped");
 auto room=Tone(AudioTapKind::Room,5000,83);
 for(int frame=83;frame<=130;++frame) { room=Tone(AudioTapKind::Room,5000,frame); balance.Process(room); }
 Require(Rms(room)>3300,"Room volume did not recover after lecturer stopped");
 RecordingAudioBalance roomOnly(false,true); room=Tone(AudioTapKind::Room,5000,1); const auto original=room.samples; roomOnly.Process(room);
 Require(room.samples==original,"Room-only recording was attenuated");
 balance.Reset(); room=Tone(AudioTapKind::Room,5000,1); balance.Process(room);
 Require(room.samples==original,"Pause reset retained old voice ducking");
 std::cout << "Recording microphone boost, noise, mute, headroom and room recovery PASS\n"; return 0;
 } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
