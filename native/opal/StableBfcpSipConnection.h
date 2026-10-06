#pragma once
#include "SessionPreservingSipConnection.h"
#include <sdp/bfcpinterface.h>
#include <codec/vidcodec.h>
#include <rtp/rtp_session.h>
#include <atomic>
#include <vector>

namespace studio::opal {
// The local BFCP server registers one presentation floor. Stock SDP allocates
// a new floor for every video stream on every offer, including main video.
class StableBfcpSipConnection : public SessionPreservingSipConnection {
public:
 explicit StableBfcpSipConnection(const Init& init):SessionPreservingSipConnection(init){}
 unsigned LocalBfcpParticipantUserId()const{return m_localBfcpParticipantUserId.load();}
protected:
 std::atomic<unsigned> m_localBfcpParticipantUserId{0};
 std::vector<unsigned> m_answerSessionIds;
 unsigned AnswerMediaSessionId(unsigned index)const{return index<m_answerSessionIds.size()?m_answerSessionIds[index]:index;}
 // Mirror OPAL's stream-index allocation before it processes the m-lines.
 // BFCP may legally precede the video whose SDP label it references.
 std::vector<unsigned> AnswerSessionIds(const SDPSessionDescription& offer){
  const auto count=offer.GetMediaDescriptions().GetSize();std::vector<unsigned> ids(count+1);unsigned next=1;
  for(PINDEX index=1;index<=count;++index){
   auto* media=offer.GetMediaDescriptionByIndex(index);if(!media)continue;
   if(media->IsGroupMember(OpalMediaSession::GetBundleGroupId())){
    for(PINDEX previous=index-1;previous>0;--previous){auto* candidate=offer.GetMediaDescriptionByIndex(previous);
     if(candidate&&candidate->GetMediaType()==media->GetMediaType()&&candidate->IsGroupMember(OpalMediaSession::GetBundleGroupId())){ids[index]=ids[previous];break;}
    }
   }
   if(!ids[index])ids[index]=next++;
  }
  return ids;
 }
 void PrepareBfcpVideoLabels(const SDPSessionDescription& offer,const std::vector<unsigned>& ids){
  bool hasBfcp=false;
  for(PINDEX index=1;index<ids.size();++index){auto* media=offer.GetMediaDescriptionByIndex(index);
   if(media&&media->GetPort()!=0&&media->GetMediaType()==BFCPMediaDefinition::Name()&&media->GetMediaFormats().HasFormat(BFCPMediaFormat::Get()))hasBfcp=true;
  }
  if(!hasBfcp)return;
  const auto supported=GetLocalMediaFormats();
  for(PINDEX index=1;index<ids.size();++index){auto* media=offer.GetMediaDescriptionByIndex(index);
   if(!media||media->GetPort()==0||media->GetMediaType()!=OpalMediaType::Video())continue;
   bool compatible=false;for(const auto& format:media->GetMediaFormats())if(supported.HasFormat(format)){compatible=true;break;}
   if(!compatible)continue;
   auto* existing=GetMediaSession(ids[index]);if(existing&&existing->GetMediaType()!=OpalMediaType::Video()&&existing->GetMediaType()!=OpalPresentationVideoMediaDefinition::Name())continue;
   auto* session=UseMediaSession(ids[index],OpalMediaType::Video(),media->GetSessionType());
   RTP_SyncSourceArray unusedSsrcs;
   if(session&&media->ToSession(session,unusedSsrcs)&&media->GetContentRole()==OpalVideoFormat::eNoRole)session->SetVideoContentRole(OpalVideoFormat::eMainRole);
  }
 }
 void TranslatePeerFloorLabels(BFCPSession* bfcp){
  if(!bfcp||bfcp->GetSetUpMode()!=OpalMediaSession::SetUpModeActive)return;
  auto map=bfcp->GetFloorStreamMap();
  for(auto& floor:map){
   if(!floor.second)continue;
   const PString label=PSTRSTRM(floor.second);unsigned matched=0,matches=0;
   for(auto it=m_sessions.begin();it!=m_sessions.end();++it){auto* rtp=dynamic_cast<OpalRTPSession*>(it->second.GetObject());if(rtp&&rtp->GetLabel()==label){matched=rtp->GetSessionID();++matches;}}
   // A numeric peer label is not a local session ID. Unknown or ambiguous
   // labels must remain unbound rather than accidentally authorizing content.
   floor.second=matches==1?matched:0;
  }
  bfcp->SetFloorStreamMap(map);
 }
 void ProjectPeerFloorLabels(SDPMediaDescription* description,BFCPSession* bfcp){
  if(!description||description->GetMediaType()!=BFCPMediaDefinition::Name()||!bfcp||bfcp->GetSetUpMode()!=OpalMediaSession::SetUpModeActive)return;
  for(const auto& floor:bfcp->GetFloorStreamMap()){
   PString value=PSTRSTRM(floor.first);
   if(floor.second){
    auto* rtp=dynamic_cast<OpalRTPSession*>(GetMediaSession(floor.second));
    const PString label=rtp&&!rtp->GetLabel().IsEmpty()?rtp->GetLabel():PSTRSTRM(floor.second);
    value+=PSTRSTRM(" mstrm:"<<label);
   }
   description->SetAttribute("floorid",value);
  }
 }
 void NormalizeVideoLabel(SDPMediaDescription* description,OpalMediaSession* session,BFCPSession* bfcp,const SDPMediaDescription* incoming=nullptr){
  if(!description||!session||description->GetMediaType()!=OpalMediaType::Video())return;
  PString label=PSTRSTRM(session->GetSessionID());
  if(!bfcp||bfcp->GetSetUpMode()==OpalMediaSession::SetUpModeActive){
   auto* rtp=dynamic_cast<OpalRTPSession*>(session);
   if(rtp&&!rtp->GetLabel().IsEmpty())label=rtp->GetLabel();
   // A bundled RTP session may have several m-lines with distinct peer labels.
   // Read the SDP shadow through a genuine copy, never a downcast to a fake
   // derived object. Keep each incoming m-line's own label when answering.
   if(auto* incomingRtp=dynamic_cast<const SDPRTPAVPMediaDescription*>(incoming)){
    struct LabelCopy:SDPRTPAVPMediaDescription{LabelCopy(const SDPRTPAVPMediaDescription& source):SDPRTPAVPMediaDescription(source){}PString Label()const{return m_label;}} copy(*incomingRtp);
    if(!copy.Label().IsEmpty())label=copy.Label();
   }
  }
  // RTP SDP has a shadow label populated from the peer's RTP session, while
  // stock also sets the base label from our session ID. Clear the shadow
  // through its virtual attribute API, then retain one readable base label.
  description->SetAttribute("label",PString::Empty());
  description->SetLabel(label);
 }
 bool OnSendAnswerSDP(const SDPSessionDescription& offer,SDPSessionDescription& answer,bool transfer)override{
  const auto ids=AnswerSessionIds(offer);
  PrepareBfcpVideoLabels(offer,ids);
  struct AnswerMappingScope{std::vector<unsigned>& value;std::vector<unsigned> previous;AnswerMappingScope(std::vector<unsigned>& target,const std::vector<unsigned>& current):value(target),previous(target){value=current;}~AnswerMappingScope(){value=std::move(previous);}} mapping(m_answerSessionIds,ids);
  if(!SessionPreservingSipConnection::OnSendAnswerSDP(offer,answer,transfer))return false;
  auto* bfcp=dynamic_cast<BFCPSession*>(FindSessionByMediaType(BFCPMediaDefinition::Name()));
  // BFCP commonly follows video in SDP; determine the final server/client
  // role only after all incoming sessions have been set up.
  for(PINDEX index=1;index<=answer.GetMediaDescriptions().GetSize();++index){
   auto* description=answer.GetMediaDescriptionByIndex(index);auto* session=GetMediaSession(ids[index]);
   NormalizeVideoLabel(description,session,bfcp,offer.GetMediaDescriptionByIndex(index));
   if(description&&session&&description->GetMediaType()==BFCPMediaDefinition::Name()){
    ProjectPeerFloorLabels(description,bfcp);
   }
  }
  return true;
 }
 void RestoreLocalPresentationFloor(BFCPSession* bfcp){
  if(!bfcp||bfcp->GetSetUpMode()==OpalMediaSession::SetUpModeActive)return;
  unsigned presentation=0;
  for(auto it=m_sessions.begin();it!=m_sessions.end();++it){
   auto session=it->second;
   if(session->GetMediaType()==OpalPresentationVideoMediaDefinition::Name()||
      session->GetVideoContentRole()==OpalVideoFormat::ePresentation){presentation=session->GetSessionID();break;}
  }
  if(!presentation)return;
  // Preserve transport, conference/user IDs and role; only the local server's
  // floor map changes. The active/client branch keeps the peer's map intact.
  bfcp->SetFloorStreamMap({{1,presentation}});
 }
 SDPMediaDescription* OnSendOfferSDPStream(OpalMediaSession* session,bool openOnly,bool bundled,unsigned index,RTP_SyncSourceId ssrc)override{
  auto* bfcp=dynamic_cast<BFCPSession*>(FindSessionByMediaType(BFCPMediaDefinition::Name()));
  RestoreLocalPresentationFloor(bfcp);
  // Prevent stock from allocating a floor in the active client's remote map,
  // too. It is the peer server's map, not a place for our video labels.
  const auto before=bfcp?bfcp->GetFloorStreamMap():BFCPDescription::FloorStreamMap();
  auto* description=SessionPreservingSipConnection::OnSendOfferSDPStream(session,openOnly,bundled,index,ssrc);
  if(bfcp)bfcp->SetFloorStreamMap(before);
  NormalizeVideoLabel(description,session,bfcp);
  ProjectPeerFloorLabels(description,bfcp);
  return description;
 }
 OpalMediaSession* SetUpMediaSession(unsigned id,const OpalMediaType& type,const SDPMediaDescription& description,
                                    OpalTransportAddress& localAddress,BundleMergeInfo& bundle)override{
  if(type!=BFCPMediaDefinition::Name())return SessionPreservingSipConnection::SetUpMediaSession(id,type,description,localAddress,bundle);
  auto* session=UseMediaSession(id,type,description.GetSessionType());
  auto* bfcp=dynamic_cast<BFCPSession*>(session);
  // BFCP has no RTP SSRCs or bundled RTP transport.
  RTP_SyncSourceArray unusedSsrcs;
  if(!bfcp||!description.ToSession(session,unusedSsrcs))return nullptr;
  RestoreLocalPresentationFloor(bfcp);
  TranslatePeerFloorLabels(bfcp);
  if(!session->Open(GetMediaInterface(),description.GetMediaAddress())){m_localBfcpParticipantUserId=0;return nullptr;}
  // OPAL applies SDP again after Open. A c-only answer often omits floorid;
  // its default empty entry must not erase the floor registered by our FCS.
  if(!description.ToSession(session,unusedSsrcs))return nullptr;
  RestoreLocalPresentationFloor(bfcp);
  TranslatePeerFloorLabels(bfcp);
  m_localBfcpParticipantUserId=bfcp->GetLocalParticipantUserID();
  localAddress=session->GetLocalAddress();
  return session;
 }
};
}
