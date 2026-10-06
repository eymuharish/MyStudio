#pragma once
#include <telestudio/media/AudioBlockFramer.h>
#include <algorithm>
#include <cmath>
namespace studio::opal {
// Applied only to the recorder's owned PCM copies, never to call audio.
// Preserve silence and low background levels; prioritize an active lecturer.
class RecordingAudioBalance {
public:
 explicit RecordingAudioBalance(bool microphone, bool room) : microphone_(microphone), room_(room) {}
 void Process(telestudio::media::OwnedAudioBlock& block) noexcept {
  if(block.samples.empty() || block.channels!=1 || block.duration100ns!=200000) return;
  double start=1, end=1;
  if(block.source==telestudio::media::AudioTapKind::Microphone && microphone_) {
   double power=0, peak=0;
   for(auto sample:block.samples) { power+=double(sample)*sample; peak=std::max(peak,std::abs(double(sample))); }
   const double rms=std::sqrt(power/block.samples.size());
   if(rms<180) return; // No boost of silence or quiet background noise.
   voiceUntil_=block.timestamp100ns+3000000; // 300ms hangover between words.
   const double desired=std::min(std::clamp(4800/rms,1.0,8.0),27000/peak);
   start=std::min(microphoneGain_,27000/peak);
   end=desired<start?desired:start+(desired-start)*0.25;
   microphoneGain_=end;
  } else if(block.source==telestudio::media::AudioTapKind::Room && room_ && microphone_) {
   start=roomGain_;
   const double desired=block.timestamp100ns<voiceUntil_?0.55:1.0;
   end=start+(desired-start)*(desired<start?0.5:0.15);
   roomGain_=end;
  } else return;
  for(size_t index=0;index<block.samples.size();++index) {
   const double gain=start+(end-start)*(index+1)/block.samples.size();
   block.samples[index]=static_cast<int16_t>(std::clamp(double(block.samples[index])*gain,-27000.0,27000.0));
  }
 }
 void Reset() noexcept { microphoneGain_=roomGain_=1; voiceUntil_=0; }
private:
 bool microphone_, room_;
 double microphoneGain_=1, roomGain_=1;
 int64_t voiceUntil_=0;
};
}
