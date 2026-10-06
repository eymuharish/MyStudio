#pragma once
#include <sip/sipcon.h>
#include <sdp/sdp.h>

namespace studio::opal {
class SessionPreservingSipConnection : public SIPConnection {
public:
 explicit SessionPreservingSipConnection(const Init& init):SIPConnection(init){}
protected:
 bool OnSendOfferSDP(SDPSessionDescription& sdp,bool openOnly)override{
  if(m_sessions.IsEmpty())return SIPConnection::OnSendOfferSDP(sdp,openOnly);
  // A queued initial re-INVITE may run before any stream opens. Stock's
  // current-session path requires an open stream, then allocates duplicate
  // sessions otherwise. Retain negotiated slots even after encoder failure.
  bool result=false;m_activeFormatList=m_remoteFormatList;
  for(SessionMap::iterator it=m_sessions.begin();it!=m_sessions.end();++it){
   const auto receive=GetMediaStream(it->first,true),send=GetMediaStream(it->first,false);
   const bool hasStreams=(receive!=NULL&&receive->IsOpen())||(send!=NULL&&send->IsOpen());
   if(OnSendOfferSDPSession(it->first,sdp,hasStreams,it->second->IsGroupMember(OpalMediaSession::GetBundleGroupId()))){
    result=true;
    if(!hasStreams){
     // No streams means no direction evidence. Use configured modes, then
     // retain the same local/remote hold constraints used by the stream path.
     auto* description=sdp.GetMediaDescriptionByIndex(sdp.GetMediaDescriptions().GetSize());
     auto direction=description->GetDirection();
     if(m_holdFromRemote)direction=SDPMediaDescription::Direction(direction&SDPMediaDescription::RecvOnly);
     if(m_holdToRemote>=eHoldOn)direction=SDPMediaDescription::Direction(direction&SDPMediaDescription::SendOnly);
     description->SetDirection(direction);
    }
   }
   else sdp.AddMediaDescription(it->second->CreateSDPMediaDescription());
  }
  m_activeFormatList=OpalMediaFormatList();
  return result&&!sdp.GetMediaDescriptions().IsEmpty();
 }
};
}
