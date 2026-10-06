#pragma once
#include <opal/call.h>
#include <opal/mediastrm.h>
#include <rtp/rtpconn.h>
#include <sdp/sdp.h>

namespace studio::opal {
// OPAL flattens both SDP video descriptions into one codec list. While applying
// one description, retain its codecs, payload types and negotiated role.
class ScopedVideoSessionRole {
 OpalMediaFormatList& formats;
 OpalMediaFormatList previous;
public:
 ScopedVideoSessionRole(OpalMediaFormatList& value,OpalVideoFormat::ContentRole role,const SDPMediaDescription* description=nullptr):formats(value),previous(value){
  // SDP without a content attribute is the ordinary main video section.
  // Its flattened codec may otherwise inherit a later presentation role.
  if(role==OpalVideoFormat::eNoRole)role=OpalVideoFormat::eMainRole;
  const bool scopedVideo=description&&description->GetMediaType()==OpalMediaType::Video();
  const auto negotiated=scopedVideo?description->GetMediaFormats():OpalMediaFormatList();
  OpalMediaFormatList selected;
  for(const auto& item:previous){
   auto copy=item;
   if(copy.IsMediaType(OpalMediaType::Video())){
    if(scopedVideo){
     auto peer=negotiated.FindFormat(item);
     if(peer==negotiated.end())continue;
     // A codec offered by the other m-line is not valid for this session.
     // Merge capabilities, then preserve this m-line's dynamic RTP number.
     if(!copy.Merge(*peer))continue;
     copy.SetPayloadType(peer->GetPayloadType());
    }
    copy.SetOptionEnum(OpalVideoFormat::ContentRoleOption(),role);
   }
   selected+=copy;
  }
  formats=selected;
 }
 ~ScopedVideoSessionRole(){formats=previous;}
 ScopedVideoSessionRole(const ScopedVideoSessionRole&)=delete;
};

inline bool IsPresentationStream(const OpalConnection& connection,const OpalMediaStream& stream){
 auto& call=connection.GetCall();
 for(PINDEX index=0;index<call.GetConnectionCount();++index){
  auto peer=call.GetConnection(index,PSafeReference);
  auto* rtp=peer?dynamic_cast<OpalRTPConnection*>(&*peer):nullptr;
  auto* session=rtp?rtp->GetMediaSession(stream.GetSessionID()):nullptr;
  if(!session)continue;
  if(session->GetMediaType()==OpalPresentationVideoMediaDefinition::Name()||session->GetVideoContentRole()==OpalVideoFormat::ePresentation)return true;
  if(session->GetVideoContentRole()==OpalVideoFormat::eMainRole)return false;
 }
 return stream.GetMediaFormat().GetOptionEnum(OpalVideoFormat::ContentRoleOption(),OpalVideoFormat::eNoRole)==OpalVideoFormat::ePresentation;
}
}
