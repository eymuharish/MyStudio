#pragma once
#include <cstddef>
#include <opal/mediafmt.h>

namespace studio::opal {
inline bool HasKnownAudioPayloadConflict(const OpalMediaFormat& source, unsigned received, const OpalMediaFormatList& formats) {
 bool matchingSource=false,matchingOther=false;
 // The connection list merges SDP sections. Audio/video use independent PT
 // namespaces; a matching audio codec must take precedence over other entries.
 for(const auto&format:formats){
  if(!format.IsMediaType(OpalMediaType::Audio())||unsigned(format.GetPayloadType())!=received)continue;
  if(format.GetName()==source.GetName())matchingSource=true;
  else matchingOther=true;
 }
 return matchingOther&&!matchingSource;
}
inline bool ShouldDropIncomingAudioPayload(std::size_t size, unsigned received, unsigned negotiated, bool knownCodecConflict = false) {
 if(size == 0 || received == 13)return false;
 if(knownCodecConflict)return true;
 if(received == negotiated)return false;
 // Dynamic RTP numbers are per-session labels, not codec identities. Let
 // OPAL's transcoder apply its bounded legacy remap policy without changing
 // the negotiated codec/profile. Keep static mappings and known conflicts strict.
 return received < 96 || received > 127 || negotiated < 96 || negotiated > 127;
}
}
