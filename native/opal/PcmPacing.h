#pragma once
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
namespace studio::opal {
// Keep sample duration at sub-millisecond precision. LC1024 at 48 kHz must
// advance by 21 1/3 ms, rather than stock raw-PCM pacing's rounded 22 ms.
class PcmPacing {
 std::mutex mutex;
 uint64_t generation=0,remainder=0;
 unsigned rate=0;
 std::chrono::steady_clock::time_point next;
public:
 void Wait(uint64_t callGeneration,size_t bytes,unsigned sampleRate,unsigned channels,size_t silenceFrameBytes=0){
  if(!callGeneration||!sampleRate||!channels)return;
  // A jitter buffer can emit an empty frame before the decoder has supplied
  // a PCM silence buffer. It still represents elapsed media time; spinning
  // here advances the jitter reader beyond arriving RTP indefinitely.
  if(!bytes)bytes=silenceFrameBytes?silenceFrameBytes:size_t(sampleRate)*channels*sizeof(int16_t)/50;
  std::scoped_lock lock(mutex);auto now=std::chrono::steady_clock::now();
  if(generation!=callGeneration||rate!=sampleRate||now>next+std::chrono::seconds(1)){generation=callGeneration;rate=sampleRate;remainder=0;next=now;}
  uint64_t numerator=uint64_t(bytes/(sizeof(int16_t)*channels))*1000000000+remainder;
  next+=std::chrono::nanoseconds(numerator/sampleRate);remainder=numerator%sampleRate;
  std::this_thread::sleep_until(next);
 }
};
}
