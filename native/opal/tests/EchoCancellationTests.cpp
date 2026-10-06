#include "../OpalEchoCanceller.h"
#include <array>
#include <deque>
#include <cmath>
#include <iostream>
#include <stdexcept>
void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(){try{
 studio::opal::EchoCanceller echo;Require(echo.Available()&&echo.Enabled(),"Echo cancellation unavailable");
 std::array<int16_t,960> speech;for(size_t n=0;n<speech.size();++n)speech[n]=int16_t(2500*std::sin(2*3.141592653589793*700*n/48000));
 auto unchanged=speech;echo.Capture(speech);Require(speech==unchanged,"Unreferenced local speech changed");
 std::deque<std::array<int16_t,960>> delayed(4);uint32_t random=12345;double echoInput=0,echoOutput=0,voiceSin=0,voiceCos=0;size_t count=0;
 for(int frame=0;frame<500;++frame){
  std::array<int16_t,960> far,microphone;
  for(auto& sample:far){random=random*1664525+1013904223;sample=int16_t((int(random>>16)-32768)/5);}
  delayed.push_back(far);auto heard=delayed.front();delayed.pop_front();
  for(size_t n=0;n<960;++n){const double near=frame>=400?2500*std::sin(2*3.141592653589793*700*(frame*960+n)/48000):0;microphone[n]=int16_t(heard[n]*0.35+near);}
  echo.Playback(far);auto original=microphone;echo.Capture(microphone);
  if(frame>=320&&frame<400)for(size_t n=0;n<960;++n){echoInput+=double(original[n])*original[n];echoOutput+=double(microphone[n])*microphone[n];}
  if(frame>=450)for(size_t n=0;n<960;++n){const double phase=2*3.141592653589793*700*(frame*960+n)/48000;voiceSin+=microphone[n]*std::sin(phase);voiceCos+=microphone[n]*std::cos(phase);++count;}
 }
 const double reduction=10*std::log10(echoInput/std::max(echoOutput,1.0)), voiceAmplitude=2*std::hypot(voiceSin,voiceCos)/count;
 Require(reduction>15,"Speaker echo was not sufficiently cancelled");
 Require(voiceAmplitude>2000&&voiceAmplitude<3000,"Double-talk damaged lecturer speech");
 echo.Enable(false);speech=unchanged;echo.Playback(unchanged);echo.Capture(speech);Require(speech==unchanged,"Disabled echo processing changed speech");
 echo.Enable(true);echo.Reset();speech=unchanged;echo.Capture(speech);Require(speech==unchanged,"New call retained an old speaker reference");
 std::cout<<"Echo reduction dB="<<reduction<<" local double-talk amplitude="<<voiceAmplitude<<" PASS\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
