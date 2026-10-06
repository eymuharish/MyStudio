#include <ptlib.h>
#include <ptlib/pprocess.h>
#include <ptlib/sockets.h>
#include <opal/manager.h>
#include <h323/h323caps.h>
#include <sip/sipep.h>
#include <sip/sipcon.h>
#include <opal/call.h>
#include <asn/h245.h>
#include <codec/vidcodec.h>
#include <sdp/bfcpinterface.h>
#include <sdp/sdp.h>
#include <rtp/rtp_session.h>
#include "../EndpointCapabilities.h"
#include "../VideoSessionRole.h"
#include "../BfcpNegotiation.h"
#include "../SessionPreservingSipConnection.h"
#include "../StableBfcpSipConnection.h"
#include <iostream>
#include <vector>
#include <atomic>
#include <thread>
static void CheckConcurrentBfcpMap(){
 BFCPSDPMediaDescription parsed(OpalTransportAddress("tcp$127.0.0.1:9"));parsed.SetAttribute("floorid","0 mstrm:3");PStringStream serialized;parsed.OutputAttributes(serialized);
 if(serialized.Find("a=floorid:0 mstrm:3")==P_MAX_INDEX)throw std::runtime_error("Parsing a wire floor ID must not invoke the local floor allocator");
 struct Description:BFCPDescription{explicit Description(unsigned value){m_floorStreamMap.clear();for(unsigned floor=1;floor<=64;++floor)m_floorStreamMap[floor]=value;}};
 const Description first(3),second(4);BFCPDescription shared(first);std::atomic<bool> start{false},bad{false};
 auto writer=[&]{while(!start.load())std::this_thread::yield();for(unsigned i=0;i<20000;++i){shared=(i%2?first:second);shared.SetFloorStreamMapping(65,5);}};
 auto reader=[&]{while(!start.load())std::this_thread::yield();for(unsigned i=0;i<20000;++i){const BFCPDescription copied(shared);const auto map=copied.GetFloorStreamMap();if(map.size()!=64&&map.size()!=65){bad=true;break;}const auto value=map.at(1);for(unsigned floor=1;floor<=64;++floor)if(map.at(floor)!=value)bad=true;shared.HasFloorID(32);shared.GetFloorStreamMap();}};
 std::thread write(writer),read1(reader),read2(reader);start=true;write.join();read1.join();read2.join();
 if(bad)throw std::runtime_error("Concurrent BFCP map copy exposed a partially replaced floor map");
 std::cout<<"BFCP concurrent assignment/set/copy/snapshot/HasFloorID coherent maps PASS\n";
}
class Process:public PProcess {PCLASSINFO(Process,PProcess);public:Process():PProcess("MyStudio","ContentCaps",1,0,ReleaseCode,0){}void Main()override{}};
class OwnedSipEndpoint : public SIPEndPoint { public: OwnedSipEndpoint(OpalManager& manager):SIPEndPoint(manager){} using OpalEndPoint::AddConnection; };
class EmptyStreamOfferConnection : public studio::opal::StableBfcpSipConnection {
public:
 std::atomic<unsigned> grantedFloor{0},connectedEvents{0};
 explicit EmptyStreamOfferConnection(const Init& init):StableBfcpSipConnection(init){m_localMediaFormats+=OpalG722;m_localMediaFormats+=OpalH264;m_localMediaFormats+=BFCPMediaFormat::Get();SetRemoteMediaFormats(m_localMediaFormats);}
 OpalMediaFormatList GetLocalMediaFormats()override{return m_localMediaFormats;}
 OpalMediaSession* CreateMediaSession(unsigned id,const OpalMediaType& type,const PString& transport)override{
  if(type!=BFCPMediaDefinition::Name())return SIPConnection::CreateMediaSession(id,type,transport);
  auto* session=new BFCPSession(OpalMediaSession::Init(*this,id,type,false),this);session->SetTransportProto("TCP/BFCP");return session;
 }
 PString GetMediaInterface()override{return "127.0.0.1";}
 bool Offer(SDPSessionDescription& sdp){if(std::getenv("MYSTUDIO_TEST_STOCK_OFFER"))return SIPConnection::OnSendOfferSDP(sdp,false);if(std::getenv("MYSTUDIO_TEST_CLOSED_STOCK_OFFER")&&!m_mediaStreams.IsEmpty())return SIPConnection::OnSendOfferSDP(sdp,true);return SessionPreservingSipConnection::OnSendOfferSDP(sdp,false);}
 bool HasNoStreams()const{return m_mediaStreams.IsEmpty();}
 void SetOwnedDirections(){m_autoStartInfo[OpalMediaType::Audio()]=OpalMediaType::TransmitReceive;m_autoStartInfo[OpalMediaType::Video()]=OpalMediaType::TransmitReceive;m_autoStartInfo[OpalPresentationVideoMediaDefinition::Name()]=OpalMediaType::Transmit;}
 void RetainClosedAudioStreams(){for(bool source:{false,true}){auto* stream=CreateMediaStream(OpalG722,1,source);if(!stream||stream->IsOpen())throw std::runtime_error("Owned closed-stream fixture failed");m_mediaStreams.SetAt(StreamKey(*stream),stream);}}
 void ClearOwnedClosedStreams(){m_mediaStreams.RemoveAll();}
 void SetOwnedHold(bool local,bool remote){m_holdToRemote=local?eHoldOn:eHoldOff;m_holdFromRemote=remote;}
 PINDEX SessionCount()const{return m_sessions.GetSize();}
 bool OnBfcpConnected()override{++connectedEvents;return true;}
 bool OnFloorRequestStatusGranted(const BFCPEvent& event)override{grantedFloor=event.m_floorID;return true;}
 bool OnFloorStatusGranted(const BFCPEvent& event)override{grantedFloor=event.m_floorID;return true;}
 bool OnFloorRequestStatusAborted(const BFCPEvent&)override{grantedFloor=0;return true;}
 bool OnFloorStatusAborted(const BFCPEvent&)override{grantedFloor=0;return true;}
 bool ApplyControlAnswer(const SDPSessionDescription& answer){bool multiple=false;return OnReceivedAnswerSDP(answer,multiple);}
 bool Answer(const SDPSessionDescription& offer,SDPSessionDescription& answer){return OnSendAnswerSDP(offer,answer,false);}
 SDPMediaDescription* OnSendAnswerSDPStream(SDPMediaDescription* offer,unsigned index,bool transfer,BundleMergeInfo& bundle)override{
  // This SDP/BFCP fixture has no capture leg or codec transcoder. Retain real
  // RTP setup and SDP conversion for inactive supported video without asking
  // the SDK to open media streams on an unpaired call.
  if((offer->GetMediaType()==OpalMediaType::Video()||offer->GetMediaType()==OpalMediaType::Audio())&&offer->GetDirection()==SDPMediaDescription::Inactive&&!offer->GetMediaFormats().IsEmpty()){
   OpalTransportAddress address;auto* session=SetUpMediaSession(AnswerMediaSessionId(index),offer->GetMediaType(),*offer,address,bundle);if(!session)return nullptr;
   auto* answer=session->CreateSDPMediaDescription();answer->SetIndex(index);answer->SetDirection(SDPMediaDescription::Inactive);for(const auto& format:offer->GetMediaFormats())answer->AddMediaFormat(format);return answer;
  }
  if(offer->GetMediaType()!=BFCPMediaDefinition::Name()){studio::opal::ScopedVideoSessionRole role(m_activeFormatList,offer->GetContentRole(),offer);return SIPConnection::OnSendAnswerSDPStream(offer,index,transfer,bundle);}
  OpalTransportAddress address;auto* session=SetUpMediaSession(AnswerMediaSessionId(index),BFCPMediaDefinition::Name(),*offer,address,bundle);if(!session)return nullptr;
  auto* answer=session->CreateSDPMediaDescription();answer->SetIndex(index);answer->SetSDPTransportType(offer->GetSDPTransportType());answer->AddMediaFormat(BFCPMediaFormat::Get());answer->SetDirection(SDPMediaDescription::SendRecv);return answer;
 }
 bool OnReceivedAnswerSDPStream(const SDPMediaDescription* answer,unsigned id,bool& multiple,BundleMergeInfo& bundle)override{
  if(id!=4)return true;
  OpalTransportAddress address;return SetUpMediaSession(id,BFCPMediaDefinition::Name(),*answer,address,bundle)!=nullptr;
 }
};
static void CheckSerializedVideoLabels(const SDPSessionDescription& sdp,const char* mainLabel,const char* presentationLabel,unsigned mainIndex=2,unsigned presentationIndex=3){
 std::map<unsigned,std::vector<PString>> labels;std::vector<PString> floorLabels;unsigned index=0;
 for(const auto& raw:sdp.Encode().Lines()){
  auto line=raw.Trim();if(line.Left(2)=="m=")++index;
  if(line.Left(8)=="a=label:")labels[index].push_back(line.Mid(8));
  if(line.Left(10)=="a=floorid:"){auto marker=line.Find("mstrm:");if(marker!=P_MAX_INDEX)floorLabels.push_back(line.Mid(marker+6).Trim());}
 }
 if(labels[mainIndex].size()!=1||labels[presentationIndex].size()!=1||labels[mainIndex][0]!=mainLabel||labels[presentationIndex][0]!=presentationLabel)
  throw std::runtime_error("Serialized video must have exactly one stable label each: main count="+std::to_string(labels[mainIndex].size())+", presentation count="+std::to_string(labels[presentationIndex].size()));
 for(const auto& floorLabel:floorLabels)if(floorLabel!=mainLabel&&floorLabel!=presentationLabel)throw std::runtime_error("Serialized BFCP floor pointer does not resolve to a single video label");
}
static std::vector<BYTE> ReceiveBfcp(PTCPSocket& peer,unsigned transaction){
 for(unsigned attempt=0;attempt<12;++attempt){
  std::vector<BYTE> packet(12);
  if(!peer.ReadBlock(packet.data(),12))throw std::runtime_error("Owned BFCP peer header receive failed");
  const unsigned body=((unsigned(packet[2])<<8)|packet[3])*4;
  if(body>4096)throw std::runtime_error("Owned BFCP response exceeded bound");
  packet.resize(12+body);
  if(body&&!peer.ReadBlock(packet.data()+12,body))throw std::runtime_error("Owned BFCP peer body receive failed");
  if(((unsigned(packet[8])<<8)|packet[9])==transaction)return packet;
 }
 throw std::runtime_error("Owned BFCP response did not match transaction");
}
static void SendBfcp(PTCPSocket& peer,unsigned primitive,unsigned transaction,unsigned floor=0){
 std::vector<BYTE> packet={0x20,BYTE(primitive),0,BYTE(floor?1:0),0,0,0,11,BYTE(transaction>>8),BYTE(transaction),0,2};
 if(floor)packet.insert(packet.end(),{5,4,BYTE(floor>>8),BYTE(floor)}); // mandatory FLOOR-ID
 if(!peer.Write(packet.data(),packet.size()))throw std::runtime_error("Owned BFCP peer send failed");
}
static bool HasBfcpAttribute(const std::vector<BYTE>& packet,unsigned type,unsigned value,size_t begin=12,size_t end=0){
 if(!end)end=packet.size();
 for(size_t p=begin;p+2<=end;){
  const unsigned attribute=packet[p]>>1,length=packet[p+1];
  if(length<2||p+length>end)throw std::runtime_error("Owned BFCP response has invalid attribute length");
  if(attribute==type&&length>=3&&packet[p+2]==value)return true;
  if((attribute==15||attribute==17||attribute==18)&&length>=4&&HasBfcpAttribute(packet,type,value,p+4,p+length))return true;
  p+=(length+3)&~size_t(3);
 }
 return false;
}
static void CheckAdvertisedFloorPeer(WORD port){
 PTCPSocket peer(port);peer.SetReadTimeout(3000);peer.SetWriteTimeout(3000);
 if(!peer.Connect("127.0.0.1"))throw std::runtime_error("Owned BFCP peer connect failed");
 SendBfcp(peer,11,70);if(ReceiveBfcp(peer,70)[1]!=12)throw std::runtime_error("Owned BFCP Hello was not acknowledged");
 SendBfcp(peer,7,71,1);if(ReceiveBfcp(peer,71)[1]!=8)throw std::runtime_error("Advertised presentation floor query was rejected");
 for(unsigned primitive:{7u,1u}){
  const unsigned transaction=primitive==7?72:73;SendBfcp(peer,primitive,transaction,77);
  auto response=ReceiveBfcp(peer,transaction);
  if(response[1]!=13||!HasBfcpAttribute(response,6,6)||HasBfcpAttribute(response,5,3))throw std::runtime_error("Unadvertised floor must return Invalid Floor ID without a false grant");
 }
 SendBfcp(peer,1,74,1);bool accepted=false,granted=false;
 for(unsigned attempt=0;attempt<8&&!granted;++attempt){
  auto response=ReceiveBfcp(peer,74);
  if(response[1]==13)throw std::runtime_error("Advertised presentation floor request was rejected");
  accepted|=HasBfcpAttribute(response,5,2);granted|=HasBfcpAttribute(response,5,3);
 }
 if(!accepted||!granted)throw std::runtime_error("Advertised floor request did not reach accepted then granted status");
 peer.Close();std::cout<<"Real BFCP TCP peer: advertised floor query/request accepted+granted; absent floor query/request rejected PASS\n";
}
static void CheckEmptyStreamOffer(OpalManager& manager){
 auto* sip=new OwnedSipEndpoint(manager);auto* call=manager.CreateCall(NULL);
 SIPConnection::Init init(*call,*sip);init.m_token="owned-empty-stream-offer";init.m_address=SIPURL("sip:test@127.0.0.1");
 auto* connection=static_cast<EmptyStreamOfferConnection*>(sip->AddConnection(new EmptyStreamOfferConnection(init)));
 struct ClosedStreamCleanup{EmptyStreamOfferConnection* connection;~ClosedStreamCleanup(){if(connection)connection->ClearOwnedClosedStreams();}} cleanup{connection};
 connection->SetOwnedDirections();
 const OpalMediaType types[]={OpalMediaType::Audio(),OpalMediaType::Video(),OpalPresentationVideoMediaDefinition::Name(),BFCPMediaDefinition::Name()};
 std::vector<WORD> ports;
 for(unsigned id=1;id<=4;++id){
  auto* session=connection->UseMediaSession(id,types[id-1],PString::Empty());
  if(!session)throw std::runtime_error("Owned four-session setup failed at session "+std::to_string(id));
  if(id==2||id==3)session->SetVideoContentRole(id==2?OpalVideoFormat::eMainRole:OpalVideoFormat::ePresentation);
  if(auto* rtp=dynamic_cast<OpalRTPSession*>(session);rtp&&(id==2||id==3))rtp->SetLabel(id==2?"11":"12");
  if(auto* bfcp=dynamic_cast<BFCPSession*>(session)){bfcp->SetConfID(11);bfcp->SetUserID(2);bfcp->SetFloorStreamMapping(1,3);}
  if(!session->Open("127.0.0.1",OpalTransportAddress(id==4?"tcp$127.0.0.1:*":"udp$127.0.0.1:9")))throw std::runtime_error("Owned session socket open failed");
  PIPSocket::Address address;WORD port=0;if(!session->GetLocalAddress().GetIpAndPort(address,port)||!port)throw std::runtime_error("Owned session has no numeric local port");ports.push_back(port);
 }
 for(unsigned cycle=0;cycle<3;++cycle){
  if(!connection->HasNoStreams())throw std::runtime_error("Empty-stream regression fabricated media streams");
  SDPSessionDescription offer(1,cycle+1,OpalTransportAddress("udp$127.0.0.1"));
  if(!connection->Offer(offer))throw std::runtime_error("Existing sessions must remain offerable before streams open");
  CheckSerializedVideoLabels(offer,"2","3");
  const auto floors=dynamic_cast<BFCPSession*>(connection->GetMediaSession(4))->GetFloorStreamMap();
  if(floors.size()!=1||floors.begin()->first!=1||floors.begin()->second!=3)throw std::runtime_error("Repeated actual SDK offer grew or remapped the local presentation floor: count="+std::to_string(floors.size()));
  if(offer.Encode().Find("a=floorid:1 mstrm:3")==P_MAX_INDEX||offer.Encode().Find("a=floorid:2")!=P_MAX_INDEX)throw std::runtime_error("SDP advertises floors absent from the single-floor server");
  if(connection->SessionCount()!=4||offer.GetMediaDescriptions().GetSize()!=4)throw std::runtime_error("Zero-stream re-INVITE duplicated or removed negotiated sessions");
  for(PINDEX index=1;index<=4;++index){auto* description=offer.GetMediaDescriptionByIndex(index);const auto expectedType=index==3?OpalMediaType::Video():types[index-1];if(description->GetMediaType()!=expectedType||description->GetPort()!=ports[index-1])throw std::runtime_error("Zero-stream re-INVITE changed slot "+std::to_string(index)+" type="+std::string(description->GetMediaType())+" port="+std::to_string(description->GetPort())+" expected="+std::to_string(ports[index-1]));}
  if(offer.GetMediaDescriptionByIndex(2)->GetContentRole()!=OpalVideoFormat::eMainRole||offer.GetMediaDescriptionByIndex(3)->GetContentRole()!=OpalVideoFormat::ePresentation)throw std::runtime_error("Zero-stream re-INVITE lost distinct main/presentation roles");
  for(PINDEX index=1;index<=3;++index)if(offer.GetMediaDescriptionByIndex(index)->GetDirection()!= (index==3?SDPMediaDescription::SendOnly:SDPMediaDescription::SendRecv))throw std::runtime_error("Zero-stream re-INVITE lost configured send/receive direction");
 }
 // A client-only answer has no authority over our local FCS floor map. The
 // actual SDK applies its missing floorid as a default 1->0 both around Open.
 SDPSessionDescription peerAnswer(1,1,OpalTransportAddress("udp$127.0.0.1"));
 const PString answerWire="v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=Owned client answer\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\nm=audio 0 RTP/AVP 9\r\na=rtpmap:9 G722/8000\r\nm=video 0 RTP/AVP 102\r\na=rtpmap:102 H264/90000\r\nm=video 0 RTP/AVP 102\r\na=rtpmap:102 H264/90000\r\nm=application 9 TCP/BFCP *\r\na=setup:active\r\na=connection:new\r\na=floorctrl:c-only\r\na=confid:11\r\na=userid:2\r\n";
 OpalMediaFormatList accepted;accepted+=OpalG722;accepted+=OpalH264;accepted+=BFCPMediaFormat::Get();
 if(!peerAnswer.Decode(answerWire,accepted)||!connection->ApplyControlAnswer(peerAnswer))throw std::runtime_error("Actual client-only BFCP answer setup failed");
 auto* ownedBfcp=dynamic_cast<BFCPSession*>(connection->GetMediaSession(4));
 if(ownedBfcp->GetFloorStreamMap()!=BFCPDescription::FloorStreamMap{{1,3}})throw std::runtime_error("Client-only answer erased the local server presentation floor");
 // Opposite role: these are the remote server's IDs. Offer generation may
 // neither normalize them to our local ID nor allocate additional entries.
 const BFCPDescription localDescription=*ownedBfcp;
 struct RemoteDescription:BFCPDescription{RemoteDescription(const BFCPDescription& original):BFCPDescription(original){m_floorStreamMap={{7,12},{8,11}};}} remoteDescription(localDescription);
 static_cast<BFCPDescription&>(*ownedBfcp)=remoteDescription;ownedBfcp->SetSetUpMode(OpalMediaSession::SetUpModeActive);
 for(unsigned cycle=0;cycle<3;++cycle){SDPSessionDescription offer(1,cycle+10,OpalTransportAddress("udp$127.0.0.1"));if(!connection->Offer(offer)||ownedBfcp->GetFloorStreamMap()!=remoteDescription.GetFloorStreamMap())throw std::runtime_error("Active BFCP client offer modified its peer server floor IDs");CheckSerializedVideoLabels(offer,"11","12");}
 static_cast<BFCPDescription&>(*ownedBfcp)=localDescription;ownedBfcp->SetSetUpMode(OpalMediaSession::SetUpModePassive);
 connection->RetainClosedAudioStreams();
 if(connection->HasNoStreams())throw std::runtime_error("Retained-closed fixture lost actual SDK streams");
 for(unsigned hold=0;hold<4;++hold){
  connection->SetOwnedHold(hold&1,hold&2);
  SDPSessionDescription offer(1,hold+4,OpalTransportAddress("udp$127.0.0.1"));
  if(!connection->Offer(offer)||connection->SessionCount()!=4||offer.GetMediaDescriptions().GetSize()!=4)throw std::runtime_error("Retained closed-stream offer lost existing sessions");
  CheckSerializedVideoLabels(offer,"2","3");
  const auto floors=dynamic_cast<BFCPSession*>(connection->GetMediaSession(4))->GetFloorStreamMap();
  if(floors.size()!=1||floors.begin()->first!=1||floors.begin()->second!=3||offer.Encode().Find("a=floorid:2")!=P_MAX_INDEX)throw std::runtime_error("Hold/retrieve offer changed the registered local floor");
  auto expected=SDPMediaDescription::SendRecv;
  if(hold&1)expected=SDPMediaDescription::Direction(expected&SDPMediaDescription::SendOnly);
  if(hold&2)expected=SDPMediaDescription::Direction(expected&SDPMediaDescription::RecvOnly);
  if(offer.GetMediaDescriptionByIndex(1)->GetDirection()!=expected)throw std::runtime_error("Retained closed audio streams lost configured direction or hold constraints");
 }
 connection->SetOwnedHold(false,false);
 connection->ClearOwnedClosedStreams();
 PString incomingWire=answerWire;incomingWire.Replace("audio 0","audio 9");incomingWire.Replace("video 0","video 9",true);
 incomingWire.Replace("a=rtpmap:9 G722/8000\r\n","a=rtpmap:9 G722/8000\r\na=inactive\r\n");
 // Peer-derived RTP labels deliberately differ from our stable session IDs.
 const PString videoMap="a=rtpmap:102 H264/90000\r\n";
 auto firstVideo=incomingWire.Find(videoMap);incomingWire.Splice("a=inactive\r\na=label:11\r\na=content:main\r\n",firstVideo+videoMap.GetLength(),0);
 auto lastVideo=incomingWire.FindLast(videoMap);incomingWire.Splice("a=inactive\r\na=label:12\r\na=content:slides\r\n",lastVideo+videoMap.GetLength(),0);
 SDPSessionDescription incoming(1,1,OpalTransportAddress("udp$127.0.0.1"));
 if(!incoming.Decode(incomingWire,accepted))throw std::runtime_error("Incoming SDK BFCP offer decode failed");
 for(unsigned cycle=0;cycle<3;++cycle){SDPSessionDescription answer(1,cycle+20,OpalTransportAddress("udp$127.0.0.1"));if(!connection->Answer(incoming,answer))throw std::runtime_error("Incoming SDK BFCP answer failed");CheckSerializedVideoLabels(answer,"2","3");if(answer.Encode().Find("a=floorid:1 mstrm:3")==P_MAX_INDEX)throw std::runtime_error("Incoming answer did not serialize the actual local presentation floor");}
 CheckAdvertisedFloorPeer(ports[3]);
 cleanup.connection=nullptr;
 for(unsigned id=1;id<=4;++id)connection->GetMediaSession(id)->Close();
 manager.ClearAllCalls(OpalConnection::EndedByLocalUser,true);
 manager.ShutDownEndpoints();
 std::cout<<"Actual four SDK sessions/no streams retain order, roles and numeric ports across three offers PASS\n";
}
static void CheckActiveClientFloor(OpalManager& manager){
 auto* sip=new OwnedSipEndpoint(manager);
 auto makeConnection=[&](const char* token){auto* call=manager.CreateCall(NULL);SIPConnection::Init init(*call,*sip);init.m_token=token;init.m_address=SIPURL("sip:test@127.0.0.1");auto* connection=static_cast<EmptyStreamOfferConnection*>(sip->AddConnection(new EmptyStreamOfferConnection(init)));connection->SetOwnedDirections();return connection;};
 auto* serverConnection=makeConnection("owned-floor7-server");
 auto* presentation=serverConnection->UseMediaSession(3,OpalPresentationVideoMediaDefinition::Name(),PString::Empty());presentation->SetVideoContentRole(OpalVideoFormat::ePresentation);
 auto* server=dynamic_cast<BFCPSession*>(serverConnection->UseMediaSession(4,BFCPMediaDefinition::Name(),PString::Empty()));
 server->SetConfID(11);server->SetUserID(2);server->SetFloorStreamMapping(7,3);
 if(!server->Open("127.0.0.1",OpalTransportAddress("tcp$127.0.0.1:*")))throw std::runtime_error("Owned remote floor7 FCS did not start");
 PIPSocket::Address address;WORD port=0;server->GetLocalAddress().GetIpAndPort(address,port);
 auto* client=makeConnection("owned-floor7-client");
 const OpalMediaType types[]={OpalMediaType::Audio(),OpalMediaType::Video(),OpalPresentationVideoMediaDefinition::Name()};
 for(unsigned id=1;id<=3;++id){auto* session=client->UseMediaSession(id,types[id-1],PString::Empty());if(id>1)session->SetVideoContentRole(id==2?OpalVideoFormat::eMainRole:OpalVideoFormat::ePresentation);}
 const PString wire=PSTRSTRM("v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=Owned remote FCS\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\nm=audio 9 RTP/AVP 9\r\na=rtpmap:9 G722/8000\r\na=inactive\r\nm=video 9 RTP/AVP 102\r\na=rtpmap:102 H264/90000\r\na=fmtp:102 packetization-mode=1\r\na=inactive\r\na=label:11\r\na=content:main\r\nm=video 9 RTP/AVP 102\r\na=rtpmap:102 H264/90000\r\na=fmtp:102 packetization-mode=1\r\na=inactive\r\na=label:12\r\na=content:slides\r\nm=application "<<port<<" TCP/BFCP *\r\na=setup:passive\r\na=connection:new\r\na=floorctrl:s-only\r\na=confid:11\r\na=userid:2\r\na=floorid:7 mstrm:12\r\n");
 OpalMediaFormatList formats;formats+=OpalG722;formats+=OpalH264;formats+=BFCPMediaFormat::Get();
 SDPSessionDescription offer(1,1,OpalTransportAddress("udp$127.0.0.1")),answer(1,1,OpalTransportAddress("udp$127.0.0.1"));
 if(!offer.Decode(wire,formats))throw std::runtime_error("Actual active-client SDP decode failed");
 // The fixture leaves video inactive to avoid device/media streams. Apply the
 // real SDK RTP SDP boundary explicitly, as active negotiated media does.
 RTP_SyncSourceArray unusedSsrcs;
 for(unsigned id:{2u,3u})if(!offer.GetMediaDescriptionByIndex(id)->ToSession(client->GetMediaSession(id),unusedSsrcs))throw std::runtime_error("Actual peer video label negotiation failed");
 if(!client->Answer(offer,answer))throw std::runtime_error("Actual active-client SDP setup failed");
 auto* control=dynamic_cast<BFCPSession*>(client->GetMediaSession(4));
 std::cerr<<"Active client negotiated labels="<<dynamic_cast<OpalRTPSession*>(client->GetMediaSession(2))->GetLabel()<<","<<dynamic_cast<OpalRTPSession*>(client->GetMediaSession(3))->GetLabel()<<'\n';
 // A connected TCP socket is published before the participant FSM commits
 // CONNECTED. Production requests are queued by OnBfcpConnected; use that
 // actual callback here too, then send one request (no retry of false maps).
 for(unsigned i=0;i<300&&(!control||!control->IsOpen()||client->connectedEvents==0);++i)PThread::Sleep(10);
 if(!control||!control->IsOpen()||client->connectedEvents==0||!control->RequestFloor(false))throw std::runtime_error("Active client could not request remote floor7 for label12/local session3");
 for(unsigned i=0;i<300&&client->grantedFloor!=7;++i)PThread::Sleep(10);
 if(client->grantedFloor!=7)throw std::runtime_error("Actual remote FCS did not grant floor7 to the active client");
 const auto connections=client->connectedEvents.load();
 for(unsigned cycle=0;cycle<3;++cycle){SDPSessionDescription next(1,cycle+2,OpalTransportAddress("udp$127.0.0.1"));if(!client->Offer(next)||!control->IsOpen()||client->connectedEvents!=connections)throw std::runtime_error("Active BFCP reoffer reopened its existing connected socket");CheckSerializedVideoLabels(next,"11","12");if(next.Encode().Find("a=floorid:7 mstrm:12")==P_MAX_INDEX)throw std::runtime_error("Active-client SDP failed to project remote floor7 to peer label12");}
 if(!control->RequestFloor(true))throw std::runtime_error("Owned client did not release floor7 before the fresh client test");
 for(unsigned i=0;i<300&&client->grantedFloor!=0;++i)PThread::Sleep(10);
 if(client->grantedFloor!=0)throw std::runtime_error("Owned client did not receive floor7 release before reusing its BFCP identity");
 control->Close();
 // Standards-valid BFCP-first SDP on fresh connections: no video session,
 // labels or synthetic media streams exist before the actual answer call.
 const auto audioStart=wire.Find("m=audio"),mainStart=wire.Find("m=video"),presentationStart=wire.Find("m=video",mainStart+1),controlStart=wire.Find("m=application");
 PString bfcpFirst=wire.Left(audioStart)+wire.Mid(controlStart)+wire.Mid(audioStart,mainStart-audioStart)+wire.Mid(mainStart,presentationStart-mainStart)+wire.Mid(presentationStart,controlStart-presentationStart);
 auto* freshActive=makeConnection("owned-fresh-bfcp-first-client");
 if(freshActive->SessionCount()!=0)throw std::runtime_error("Fresh BFCP-first fixture fabricated sessions");
 SDPSessionDescription firstOffer(1,6,OpalTransportAddress("udp$127.0.0.1")),firstAnswer(1,6,OpalTransportAddress("udp$127.0.0.1"));
 if(!firstOffer.Decode(bfcpFirst,formats)||!freshActive->Answer(firstOffer,firstAnswer))throw std::runtime_error("Fresh BFCP-first active answer failed");
 auto* firstControl=dynamic_cast<BFCPSession*>(freshActive->GetMediaSession(1));
 for(unsigned id=1;id<=4;++id)if(auto* session=freshActive->GetMediaSession(id)){auto* rtp=dynamic_cast<OpalRTPSession*>(session);std::cout<<"BFCP-first session "<<id<<" type="<<session->GetMediaType()<<" role="<<session->GetVideoContentRole()<<" label="<<(rtp?rtp->GetLabel():PString())<<'\n';}
 if(firstControl)for(const auto& floor:firstControl->GetFloorStreamMap())std::cout<<"BFCP-first floor "<<floor.first<<" session="<<floor.second<<'\n';
 for(unsigned i=0;i<300&&(!firstControl||!firstControl->IsOpen()||freshActive->connectedEvents==0);++i)PThread::Sleep(10);
 if(!firstControl||!firstControl->IsOpen()||freshActive->connectedEvents==0||firstControl->GetFloorStreamMap().at(7)!=4||!firstControl->RequestFloor(false))throw std::runtime_error("Fresh BFCP-first SDP did not resolve floor7/label12/session4 before opening the client");
 for(unsigned i=0;i<300&&freshActive->grantedFloor!=7;++i)PThread::Sleep(10);
 if(freshActive->grantedFloor!=7)throw std::runtime_error("Fresh BFCP-first active client did not receive real floor7 grant");
 CheckSerializedVideoLabels(firstAnswer,"11","12",3,4);
 auto* freshPassive=makeConnection("owned-fresh-bfcp-first-server");
 manager.SetTCPPorts(46400,46420); // A distinct FCS listener, not the earlier owned peer.
 PString passiveFirst=bfcpFirst;passiveFirst.Replace(PSTRSTRM("m=application "<<port),"m=application 9");passiveFirst.Replace("a=setup:passive","a=setup:active");passiveFirst.Replace("a=floorctrl:s-only","a=floorctrl:c-only");passiveFirst.Replace("a=floorid:7 mstrm:12\r\n",PString::Empty());
 SDPSessionDescription passiveOffer(1,7,OpalTransportAddress("udp$127.0.0.1")),passiveAnswer(1,7,OpalTransportAddress("udp$127.0.0.1"));
 if(!passiveOffer.Decode(passiveFirst,formats)||!freshPassive->Answer(passiveOffer,passiveAnswer))throw std::runtime_error("Fresh BFCP-first passive answer failed");
 auto* passiveControl=dynamic_cast<BFCPSession*>(freshPassive->GetMediaSession(1));
 if(!passiveControl||passiveControl->GetFloorStreamMap()!=BFCPDescription::FloorStreamMap{{1,4}}||passiveAnswer.Encode().Find("a=floorid:1 mstrm:4")==P_MAX_INDEX)throw std::runtime_error("Fresh BFCP-first server opened without its presentation floor");
 CheckSerializedVideoLabels(passiveAnswer,"3","4",3,4);
 PIPSocket::Address passiveAddress;WORD passivePort=0;passiveControl->GetLocalAddress().GetIpAndPort(passiveAddress,passivePort);CheckAdvertisedFloorPeer(passivePort);
 PString unknownWire=wire;unknownWire.Replace("a=floorid:7 mstrm:12","a=floorid:8 mstrm:3");
 SDPSessionDescription unknown(1,5,OpalTransportAddress("udp$127.0.0.1")),unknownAnswer(1,5,OpalTransportAddress("udp$127.0.0.1"));
 if(!unknown.Decode(unknownWire,formats)||!client->Answer(unknown,unknownAnswer))throw std::runtime_error("Unknown-label peer SDP fixture failed");
 if(control->GetFloorStreamMap().at(8)!=0||control->RequestFloor(false))throw std::runtime_error("Unknown peer label3 must not alias local presentation session3 or request a false floor");
 manager.SetTCPPorts(46200,46220);
 for(auto* connection:{client,serverConnection,freshActive,freshPassive})for(unsigned id=1;id<=4;++id)if(auto* session=connection->GetMediaSession(id))session->Close();
 manager.ClearAllCalls(OpalConnection::EndedByLocalUser,true);manager.ShutDownEndpoints();
 std::cout<<"Actual active BFCP client translated peer label12/session3, requested/granted remote floor7 and retained connected socket over three offers PASS\n";
}
class FloorCallbacks : public BFCPCallback {
public:
 BFCPSession* session=nullptr; bool disconnectedWhileOpen=false;
 bool OnBfcpConnected() override { return true; }
 bool OnBfcpDisconnected() override { disconnectedWhileOpen |= session && session->IsOpen(); return true; }
 bool OnFloorRequestStatusAccepted(const BFCPEvent&) override { return true; }
 bool OnFloorRequestStatusGranted(const BFCPEvent&) override { return true; }
 bool OnFloorRequestStatusAborted(const BFCPEvent&) override { return true; }
 bool OnFloorStatusAccepted(const BFCPEvent&) override { return true; }
 bool OnFloorStatusGranted(const BFCPEvent&) override { return true; }
 bool OnFloorStatusAborted(const BFCPEvent&) override { return true; }
};
static void CheckBfcpSocketState(OpalManager& manager) {
 auto* sip = new OwnedSipEndpoint(manager);
 OpalCall* call = manager.CreateCall(NULL);
 {
  SIPConnection::Init init(*call,*sip); init.m_token="owned-bfcp-state";
  init.m_address=SIPURL("sip:test@127.0.0.1");
  auto* connection=static_cast<SIPConnection*>(sip->AddConnection(new SIPConnection(init))); FloorCallbacks callbacks;
  BFCPSession session(OpalMediaSession::Init(*connection,4,BFCPMediaDefinition::Name(),false),&callbacks);
  callbacks.session=&session;
  session.SetTransportProto("TCP/BFCP"); session.SetConfID(11); session.SetUserID(2);
  if(session.IsOpen()) throw std::runtime_error("Uninitialized BFCP session must be closed");
  if(session.Open("127.0.0.1",OpalTransportAddress("tcp$127.0.0.1:0")) || session.IsOpen())
   throw std::runtime_error("Invalid BFCP target must stay closed");
  session.SetFloorStreamMapping(2,3);
  bool opened=session.Open("127.0.0.1",OpalTransportAddress("tcp$127.0.0.1:*"));
  std::cerr << "BFCP passive socket result=" << opened << " sessionOpen=" << session.IsOpen() << std::endl;
  if(!opened || !session.IsOpen())
   throw std::runtime_error("Successfully connected BFCP owned sockets must report open");
  if(!session.Close() || session.IsOpen() || !session.Close())
   throw std::runtime_error("BFCP Close must join sockets and remain closed/idempotent");
  if(callbacks.disconnectedWhileOpen) throw std::runtime_error("Disconnect callback must already observe closed BFCP state");
  if(!session.Open("127.0.0.1",OpalTransportAddress("tcp$127.0.0.1:*")) || !session.IsOpen())
   throw std::runtime_error("BFCP sockets must safely reopen after close");
  session.Close();
  session.SetSetUpMode(OpalMediaSession::SetUpModeActive);
  if(session.Open("127.0.0.1",OpalTransportAddress("tcp$127.0.0.1:1")) || session.IsOpen())
   throw std::runtime_error("Failed BFCP active socket must stay closed");
  session.Close();
 }
 manager.ClearAllCalls(OpalConnection::EndedByLocalUser,true);
 std::cout << "BFCP real socket open/close/failure state PASS\n";
}
static void CheckVideoRoleMerge(){
 OpalMediaFormat main=OpalH264;main.SetOptionEnum(OpalVideoFormat::ContentRoleOption(),OpalVideoFormat::eMainRole);
 OpalMediaFormatList active;active+=main;
 {
  studio::opal::ScopedVideoSessionRole role(active,OpalVideoFormat::ePresentation);
  auto encoded=active.front();OpalMediaFormat raw=OpalYUV420P;
  if(!raw.Merge(encoded,true)||raw.GetOptionEnum(OpalVideoFormat::ContentRoleOption(),0)!=OpalVideoFormat::ePresentation)
   throw std::runtime_error("Actual SDP presentation role must survive raw/encoded update merge");
 }
 if(active.front().GetOptionEnum(OpalVideoFormat::ContentRoleOption(),0)!=OpalVideoFormat::eMainRole)
  throw std::runtime_error("Scoped presentation merge must not change main video capability");
 OpalMediaFormat presentation=OpalH264;presentation.SetOptionEnum(OpalVideoFormat::ContentRoleOption(),OpalVideoFormat::ePresentation);
 OpalMediaFormatList offered;offered+=presentation;
 SDPSessionDescription implicitMain(1,1,OpalTransportAddress("udp$127.0.0.1"));
 if(!implicitMain.Decode(PString("v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=Main without content\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\nm=video 6000 RTP/AVP 102\r\na=rtpmap:102 H264/90000\r\n"),offered))
  throw std::runtime_error("Implicit-main SDP could not decode");
 {
  studio::opal::ScopedVideoSessionRole role(offered,implicitMain.GetMediaDescriptionByIndex(1)->GetContentRole());
  OpalMediaFormat raw=OpalYUV420P;
  if(!raw.Merge(offered.front(),true)||raw.GetOptionEnum(OpalVideoFormat::ContentRoleOption(),0)!=OpalVideoFormat::eMainRole)
   throw std::runtime_error("SDP without content inherited flattened presentation role");
 }
 if(offered.front().GetOptionEnum(OpalVideoFormat::ContentRoleOption(),0)!=OpalVideoFormat::ePresentation)
  throw std::runtime_error("Implicit main scope damaged presentation capability");
 std::cout << "SDP presentation and implicit main merge retain distinct roles PASS\n";
 // The same codec may have different dynamic numbers, or each video m-line
 // may select a different packetization mode. Neither may leak across scopes.
 for(bool sameCodec:{false,true}){
  OpalMediaFormatList supported;supported+=OpalH264;supported+=OpalH264_MODE0;supported+=OpalG722;
  const PString wire=PSTRSTRM("v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=Per-session video\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\nm=audio 6000 RTP/AVP 9\r\na=rtpmap:9 G722/8000\r\nm=video 6002 RTP/AVP 98\r\na=rtpmap:98 H264/90000\r\na=fmtp:98 packetization-mode=1\r\na=content:main\r\nm=video 6004 RTP/AVP 120\r\na=rtpmap:120 H264/90000\r\na=fmtp:120 packetization-mode="<<(sameCodec?1:0)<<"\r\na=content:slides\r\n");
  SDPSessionDescription sdp(1,1,OpalTransportAddress("udp$127.0.0.1"));
  if(!sdp.Decode(wire,supported))throw std::runtime_error("Per-session video SDP could not decode");
  auto active=sdp.GetMediaFormats();const auto flattened=active;
  for(unsigned index:{2u,3u,3u,2u}){
   auto* description=sdp.GetMediaDescriptionByIndex(index);auto expected=description->GetMediaFormats().front();
   {
    studio::opal::ScopedVideoSessionRole scope(active,description->GetContentRole(),description);
    unsigned count=0;for(const auto& format:active)if(format.IsMediaType(OpalMediaType::Video())){
     ++count;if(format.GetName()!=expected.GetName()||format.GetPayloadType()!=expected.GetPayloadType()||format.GetOptionEnum(OpalVideoFormat::ContentRoleOption(),0)!=description->GetContentRole())
      throw std::runtime_error("Per-session video scope leaked another codec, RTP number or role");
    }
    if(count!=1||!active.HasFormat(OpalG722))throw std::runtime_error("Per-session video scope removed audio or retained unnegotiated video");
   }
   if(active.GetSize()!=flattened.GetSize())throw std::runtime_error("Video scope did not restore the flattened list");
  }
 }
 std::cout<<"SDP per-session codecs, dynamic payload numbers and roles across repeated scopes PASS\n";
}
static void CheckBfcpNegotiationRollback(){
 OpalMediaFormatList formats;formats+=BFCPMediaFormat::Get();
 const PString activeWire="v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=Owned BFCP negotiation\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\nm=application 6000 TCP/BFCP *\r\na=setup:actpass\r\na=connection:new\r\na=floorctrl:s-only\r\na=confid:1\r\na=userid:2\r\na=floorid:1 mstrm:3\r\n";
 PString disabledWire=activeWire;disabledWire.Replace("application 6000","application 0");
 SDPSessionDescription active(1,1,OpalTransportAddress("udp$127.0.0.1")),disabled(1,2,OpalTransportAddress("udp$127.0.0.1"));
 if(!active.Decode(activeWire,formats)||!disabled.Decode(disabledWire,formats))throw std::runtime_error("Owned enabled/disabled BFCP SDP decode failed");
 SIP_PDU bodyless;std::atomic<bool> negotiated=false;
 auto apply=[&](const SDPSessionDescription* sdp,bool accepted){
  studio::opal::BfcpNegotiationTransaction update(negotiated,sdp);
  negotiated=false; // A stream negotiation can invalidate the provisional candidate.
  if(sdp)for(PINDEX i=1;i<=sdp->GetMediaDescriptions().GetSize();++i){auto*description=sdp->GetMediaDescriptionByIndex(i);if(description->GetMediaType()==BFCPMediaDefinition::Name()&&description->GetPort()!=0&&description->GetMediaFormats().HasFormat(BFCPMediaFormat::Get()))negotiated=true;}
  update.Complete(accepted);
 };
 for(unsigned cycle=0;cycle<3;++cycle){
  apply(&active,true);if(!negotiated)throw std::runtime_error("Accepted BFCP SDP did not commit session");
  apply(bodyless.GetSDP(),false);if(!negotiated)throw std::runtime_error("Bodyless SIP response erased established BFCP session");
  apply(bodyless.GetSDP(),true);if(!negotiated)throw std::runtime_error("Response without SDP must not replace BFCP state");
  apply(&disabled,false);if(!negotiated)throw std::runtime_error("Rejected SDP removed previously negotiated BFCP");
  try{studio::opal::BfcpNegotiationTransaction update(negotiated,&disabled);throw 1;}catch(int){}
  if(!negotiated)throw std::runtime_error("Aborted SDP processing removed previous BFCP session");
  apply(&disabled,true);if(negotiated)throw std::runtime_error("Accepted port-zero BFCP must disable negotiation");
 }
 std::cout<<"BFCP real SDP accepted/bodyless/rejected rollback and renegotiation PASS\n";
}
int main(){_putenv_s("PTLIBPLUGINDIR","MyStudio-no-auto-plugins");Process process;try{CheckConcurrentBfcpMap();CheckVideoRoleMerge();CheckBfcpNegotiationRollback();OpalMediaFormatList formats;formats+=BFCPMediaFormat::Get();
for(const char*transport:{"TCP/BFCP","UDP/BFCP"}){PString offer=PSTRSTRM("v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=Owned BFCP\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\nm=application 6000 "<<transport<<" *\r\na=setup:actpass\r\na=connection:new\r\na=floorctrl:s-only\r\na=confid:1\r\na=userid:2\r\na=floorid:1 mstrm:3\r\n");SDPSessionDescription decoded(1,1,OpalTransportAddress("udp$127.0.0.1"));if(!decoded.Decode(offer,formats)||!decoded.GetMediaFormats().HasFormat(BFCPMediaFormat::Get())||decoded.GetMediaDescriptionByIndex(1)->GetMediaType()!=BFCPMediaDefinition::Name()||decoded.Encode().Find(PSTRSTRM("m=application 6000 "<<transport<<" *"))==P_MAX_INDEX||decoded.Encode().Find("a=rtpmap:")!=P_MAX_INDEX)throw std::runtime_error("BFCP wildcard SDP must retain its actual registered control format");SDPSessionDescription disabled(1,1,OpalTransportAddress("udp$127.0.0.1"));disabled.Decode(offer,OpalMediaFormatList());if(disabled.GetMediaFormats().HasFormat(BFCPMediaFormat::Get()))throw std::runtime_error("Unsupported BFCP control must stay rejected");}
OpalManager manager;PTrace::Initialise(5,"bfcp-owned-sdk-trace.log",PTrace::Timestamp|PTrace::Thread|PTrace::FileAndLine);manager.SetTCPPorts(46200,46220);manager.SetRtpIpPorts(49000,49198);CheckEmptyStreamOffer(manager);CheckActiveClientFloor(manager);CheckBfcpSocketState(manager);auto*endpoint=new H323EndPoint(manager);studio::opal::ConfigureH323Content(*endpoint);if(!endpoint->GetDefaultH239Control())throw std::runtime_error("H239 token control must be enabled before the first TCS");H323H239ControlCapability sender,receiver;H245_Capability wire;if(!sender.OnSendingPDU(wire)||!receiver.OnReceivedPDU(wire))throw std::runtime_error("H239 actual generic control capability roundtrip failed");manager.ShutDownEndpoints();std::cout<<"H239 configured token control and real capability PDU PASS\n";return 0;}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
