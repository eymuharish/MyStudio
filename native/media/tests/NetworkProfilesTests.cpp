#include "telestudio/media/NetworkProfiles.h"
#include <stdexcept>
#include <iostream>
using namespace telestudio::media;
int main() {
  try {
    auto require=[](bool ok){if(!ok)throw std::runtime_error("network profile invariant failed");};
    const auto good=ParseAudioNetworkProfile({"audio-profile","aac-ld","1","64","100","600","200","150","0"});
    require(good.has_value());require(ValidateAudioNetworkProfile(*good,true).empty());
    auto stereo=*good;stereo.channels=2;stereo.bitrateKbps=128;
    require(ValidateAudioNetworkProfile(stereo,true).empty());require(ValidateAudioNetworkProfile(stereo,false).empty());
    auto monoLc=*good;monoLc.codec="aac-lc";monoLc.bitrateKbps=96;
    require(ValidateAudioNetworkProfile(monoLc,true).empty());
    auto rounded=*good;rounded.packetMs=10;require(!ValidateAudioNetworkProfile(rounded,true).empty());
    require(!ParseAudioNetworkProfile({"audio-profile","g722","1","64","200","100","100","40","0"}));
    require(!ParseAudioNetworkProfile({"audio-profile","g711","1","64","100","600","200","40","0"}));
    require(ParseNetworkVideoProfile({"network-video","2","384","text"}).has_value());
    require(!ParseNetworkVideoProfile({"network-video","0","384","text"}));
    require(!ParseNetworkVideoProfile({"network-video","30","9999999","text"}));
    const auto event=AudioProfileApplied({1,ConsoleProtocol::Sip},*good);
    require(event && event->find("\"sendBufferMs\":160")!=std::string::npos);
    std::cout<<"network_profiles_tests: PASS\n";return 0;
  } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
