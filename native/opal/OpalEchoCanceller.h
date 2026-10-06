#pragma once
#include <cstdint>
#include <span>
#include <array>
#include <algorithm>
#include <speex/speex_echo.h>
namespace studio::opal {
class EchoCanceller {
public:
 EchoCanceller() noexcept {
  state_=speex_echo_state_init(960,14400); // 20ms blocks, 300ms including device queue.
  int rate=48000;
  if(state_ && speex_echo_ctl(state_,SPEEX_ECHO_SET_SAMPLING_RATE,&rate)!=0) { speex_echo_state_destroy(state_); state_=nullptr; }
 }
 ~EchoCanceller() { if(state_)speex_echo_state_destroy(state_); }
 EchoCanceller(const EchoCanceller&)=delete;
 EchoCanceller& operator=(const EchoCanceller&)=delete;
 bool Available() const noexcept { return state_!=nullptr; }
 bool Enabled() const noexcept { return enabled_&&Available(); }
 void Enable(bool value) noexcept { if(enabled_!=value)Reset(); enabled_=value; }
 void Reset() noexcept { if(state_)speex_echo_state_reset(state_); referenced_=false; }
 void Playback(std::span<const int16_t> samples) noexcept {
  if(!Enabled()||samples.size()!=960)return;
  speex_echo_playback(state_,samples.data());referenced_=true;
 }
 void Capture(std::span<int16_t> samples) noexcept {
  if(!Enabled()||!referenced_||samples.size()!=960)return;
  std::array<int16_t,960> clean;
  speex_echo_capture(state_,samples.data(),clean.data());
  std::copy(clean.begin(),clean.end(),samples.begin());
 }
private:
 SpeexEchoState* state_=nullptr;
 bool enabled_=true,referenced_=false;
};
}
