#pragma once
#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "ConsoleEventWriter.h"

namespace telestudio::media {
struct AudioNetworkProfile {
  std::string codec = "auto";
  unsigned channels = 1, bitrateKbps = 64;
  unsigned jitterMinimumMs = 100, jitterMaximumMs = 300, jitterInitialMs = 100;
  unsigned sendBufferMs = 40, packetMs = 0;
};
struct NetworkVideoProfile { unsigned fps = 15, bitrateKbps = 2000; bool text = true; };
inline bool ProfileNumber(std::string_view value, unsigned& result) {
  const auto parsed = std::from_chars(value.data(), value.data()+value.size(), result);
  return !value.empty() && parsed.ec == std::errc{} && parsed.ptr == value.data()+value.size();
}
inline std::optional<AudioNetworkProfile> ParseAudioNetworkProfile(const std::vector<std::string_view>& t) {
  if(t.size()!=9 || t[0]!="audio-profile") return {};
  AudioNetworkProfile p; p.codec=t[1];
  if(p.codec!="auto" && p.codec!="aac-ld" && p.codec!="aac-lc" && p.codec!="g722") return {};
  unsigned* fields[]{&p.channels,&p.bitrateKbps,&p.jitterMinimumMs,&p.jitterMaximumMs,&p.jitterInitialMs,&p.sendBufferMs,&p.packetMs};
  for(unsigned i=0;i<7;++i) if(!ProfileNumber(t[i+2],*fields[i])) return {};
  if(p.channels<1 || p.channels>2 || p.bitrateKbps<32 || p.bitrateKbps>192 ||
     p.jitterMinimumMs<100 || p.jitterMaximumMs>600 || p.jitterMinimumMs>p.jitterInitialMs || p.jitterInitialMs>p.jitterMaximumMs ||
     p.jitterMinimumMs%50 || p.jitterMaximumMs%50 || p.jitterInitialMs%50) return {};
  if(p.sendBufferMs!=20 && p.sendBufferMs!=40 && p.sendBufferMs!=60 && p.sendBufferMs!=100 && p.sendBufferMs!=150 && p.sendBufferMs!=200) return {};
  if(p.packetMs>120) return {};
  return p;
}
inline std::optional<NetworkVideoProfile> ParseNetworkVideoProfile(const std::vector<std::string_view>& t) {
  if(t.size()!=4 || t[0]!="network-video") return {};
  NetworkVideoProfile p;
  if(!ProfileNumber(t[1],p.fps) || !ProfileNumber(t[2],p.bitrateKbps)) return {};
  if(p.fps!=2 && p.fps!=5 && p.fps!=10 && p.fps!=15 && p.fps!=24 && p.fps!=30) return {};
  if(p.bitrateKbps<128 || p.bitrateKbps>20000 || (t[3]!="text" && t[3]!="motion")) return {};
  p.text=t[3]=="text";return p;
}
// Native AAC encoders preserve fractional frame timing. Integer packet override
// must never round their 512/48000 or 1024/48000 frame duration.
inline std::string ValidateAudioNetworkProfile(const AudioNetworkProfile& p, bool sip) {
  if(p.packetMs!=0 && (p.codec!="g722" || p.packetMs!=20)) return "unsupported-packet-duration";
  if(p.codec=="g722" && (p.channels!=1 || p.bitrateKbps!=64)) return "unsupported-g722-format";
  if(p.codec=="aac-ld" && (p.channels<1 || p.channels>2 || p.bitrateKbps!=64*p.channels)) return "unsupported-aac-ld-format";
  if(p.codec=="aac-lc" && (p.channels<1 || p.channels>2 || p.bitrateKbps!=96*p.channels)) return "unsupported-aac-lc-format";
  return {};
}
inline std::optional<std::string> AudioProfileApplied(EventPrefix e,const AudioNetworkProfile& p) {
  return "{\"schema\":1,\"type\":\"audio-profile-applied\",\"sequence\":"+std::to_string(e.sequence)+",\"protocol\":\""+(e.protocol==ConsoleProtocol::Sip?"sip":"h323")+
    "\",\"codec\":\""+p.codec+"\",\"channels\":"+std::to_string(p.channels)+",\"bitrateKbps\":"+std::to_string(p.bitrateKbps)+
    ",\"jitterMinimumMs\":"+std::to_string(p.jitterMinimumMs)+",\"jitterMaximumMs\":"+std::to_string(p.jitterMaximumMs)+",\"jitterInitialMs\":"+std::to_string(p.jitterInitialMs)+
    ",\"sendBufferMs\":"+std::to_string((p.sendBufferMs+19)/20*20)+",\"packetMs\":"+std::to_string(p.packetMs)+",\"negotiated\":false}";
}
inline std::optional<std::string> NetworkVideoApplied(EventPrefix e,const NetworkVideoProfile& p) {
  return "{\"schema\":1,\"type\":\"network-video-applied\",\"sequence\":"+std::to_string(e.sequence)+",\"protocol\":\""+(e.protocol==ConsoleProtocol::Sip?"sip":"h323")+
    "\",\"fps\":"+std::to_string(p.fps)+",\"bitrateKbps\":"+std::to_string(p.bitrateKbps)+",\"content\":\""+(p.text?"text":"motion")+"\",\"negotiated\":false}";
}
inline std::optional<std::string> AudioDirectionState(EventPrefix e,bool send,AudioCodecName codec,int sampleRate,int channels,int bitrateKbps) {
  const std::string name=codec==AudioCodecName::AacLd ? "aac-ld" : codec==AudioCodecName::AacLc ? "aac-lc" : "g722";
  return "{\"schema\":1,\"type\":\"audio-direction-state\",\"sequence\":"+std::to_string(e.sequence)+",\"protocol\":\""+(e.protocol==ConsoleProtocol::Sip?"sip":"h323")+
    "\",\"direction\":\""+(send?"send":"receive")+"\",\"codec\":\""+name+"\",\"sampleRate\":"+std::to_string(sampleRate)+",\"channels\":"+std::to_string(channels)+",\"bitrateKbps\":"+std::to_string(bitrateKbps)+"}";
}
}
