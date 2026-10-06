#include <ptlib.h>
#include "EndpointCapabilities.h"
#include <ptlib/pprocess.h>
#include <ptlib/pluginmgr.h>
#include <opal/manager.h>
#include <opal/call.h>
#include <ep/localep.h>
#include <sip/sipep.h>
#include <sip/sipcon.h>
#include <h323/h323ep.h>
#include <h323/h323con.h>
#include <codec/vidcodec.h>
#include <opal/patch.h>
#include <rtp/rtp_stream.h>
#include <sdp/bfcpinterface.h>
#include "OpalMedia.h"
#include "OpalH264.h"
#include "WorkerPolicy.h"
#include "AudioPreferences.h"
#include "PcmPacing.h"
#include "IncomingAudioPayload.h"
#include "PcmMediaStream.h"
#include "VideoPreferences.h"
#include "VideoSessionRole.h"
#include "BfcpNegotiation.h"
#include "SessionPreservingSipConnection.h"
#include "StableBfcpSipConnection.h"
#include "CrashDiagnostics.h"
#include "aac/OpalAacHandler.h"
#include "aac/AacSdpSessionDescription.h"
#include <sdp/sdp.h>
#include "SipCommandParser.h"
#include <telestudio/media/ConsoleEventWriter.h>
#include <windows.h>
#include <iostream>
#include <sstream>
#include <thread>
#include <deque>
#include <condition_variable>
#include <algorithm>
#include <tuple>
using namespace telestudio::sip;
namespace studio::opal {
std::string env(const char*key){auto*p=std::getenv(key);return p?p:"";}
std::string str(const PString&s){return std::string(static_cast<const char*>(s));}
std::string quote(std::string_view value){std::string s="\"";for(unsigned char c:value){if(c=='"'||c=='\\')s+='\\';s+=c<32?' ':c;}return s+'"';}
std::string decode(std::string_view value){if(value.empty()||value.size()%4==1)throw std::runtime_error("Invalid encoded argument");std::string out;uint32_t bits=0;unsigned count=0;for(char c:value){int d=c>='A'&&c<='Z'?c-'A':c>='a'&&c<='z'?c-'a'+26:c>='0'&&c<='9'?c-'0'+52:c=='-'?62:c=='_'?63:-1;if(d<0)throw std::runtime_error("Invalid encoded argument");bits=bits<<6|d;count+=6;if(count>=8){count-=8;char item=char(bits>>count);if(!item)throw std::runtime_error("Invalid encoded argument");out+=item;}}if(count&&(bits&((1u<<count)-1)))throw std::runtime_error("Invalid encoded argument");return out;}
std::filesystem::path bin(){std::wstring s(32768,L'\0');auto n=GetModuleFileNameW(nullptr,s.data(),DWORD(s.size()));s.resize(n);return std::filesystem::path(s).parent_path();}
bool ReadCommand(std::istream&input,std::string&line){line.clear();bool overlong=false,read=false;char ch;while(input.get(ch)){read=true;if(ch=='\n')break;if(line.size()<4096)line+=ch;else overlong=true;}if(overlong)line="invalid-overlong";if(!line.empty()&&line.back()=='\r')line.pop_back();return read;}
bool loopback(std::string address){auto at=address.rfind('@');if(at!=std::string::npos)address.erase(0,at+1);for(auto prefix:{"sip:","h323:"})if(address.starts_with(prefix))address.erase(0,std::strlen(prefix));auto semi=address.find(';');if(semi!=std::string::npos)address.erase(semi);return address=="localhost"||address.starts_with("localhost:")||address=="127.0.0.1"||address.starts_with("127.0.0.1:")||address=="[::1]"||address.starts_with("[::1]:");}
class StudioManager;
class StudioLocal : public OpalLocalEndPoint {
 StudioManager& studio;
 PcmPacing sendPacing;
public:
 explicit StudioLocal(StudioManager&);
 OpalLocalConnection*CreateConnection(OpalCall&call,void*data,unsigned options,OpalConnection::StringOptions*strings)override{return new PcmLocalConnection(call,*this,data,options,strings);}
 bool OnIncomingCall(OpalLocalConnection&)override;
 bool OnReadMediaData(OpalLocalConnection&,OpalMediaStream&,void*,PINDEX,PINDEX&)override;
 bool OnWriteMediaData(const OpalLocalConnection&,const OpalMediaStream&,const void*,PINDEX,PINDEX&)override;
 bool OnReadMediaFrame(const OpalLocalConnection&,const OpalMediaStream&,RTP_DataFrame&)override;
 bool OnWriteMediaFrame(const OpalLocalConnection&,const OpalMediaStream&,RTP_DataFrame&)override;
};
bool RejectShortSipSession(SIP_PDU&request){const auto&mime=request.GetMIME();auto minimum=std::max(PTimeInterval(0,90),mime.GetMinSE());if(!mime.Has("Session-Expires")||mime.GetSessionExpires()>=minimum)return false;SIP_PDU response(request,SIP_PDU::Failure_SessionIntervalTooSmall);response.GetMIME().SetMinSE(minimum);auto to=response.GetMIME().GetTo();if(to.GetTag().IsEmpty())to.SetTag();response.GetMIME().SetTo(to);response.Send();return true;}
std::string SipSessionRefresher(const SIPMIMEInfo&mime){auto fields=mime.GetString("Session-Expires").Tokenise(';',false);for(PINDEX i=1;i<fields.GetSize();++i){auto equal=fields[i].Find('=');if(equal==P_MAX_INDEX||PCaselessString(fields[i].Left(equal).Trim())!="refresher")continue;PCaselessString value(fields[i].Mid(equal+1).Trim());if(value=="uac")return "uac";if(value=="uas")return "uas";}return {};}
class StudioSip : public SIPEndPoint {
 StudioManager& studio;
public:explicit StudioSip(StudioManager&);
 SDPSessionDescription* CreateSDP(time_t id,unsigned version,const OpalTransportAddress&address)override{return new AacSdpSessionDescription(id,version,address);}
 void OnRegistrationStatus(const RegistrationStatus&)override;
 SIPConnection* CreateConnection(const SIPConnection::Init&)override;
 bool OnReceivedPDU(SIP_PDU*pdu)override{
  // RFC3261 8.2.7: ACK never receives a response. OPAL otherwise replies481
  // for the ACK of a stateless422 which deliberately allocated no connection.
  if(pdu->GetMethod()==SIP_PDU::Method_ACK&&GetSIPConnectionWithLock(pdu->GetMIME().GetToTag(),PSafeReference)==NULL&&GetSIPConnectionWithLock(pdu->GetMIME().GetFromTag(),PSafeReference)==NULL)return false;
  return SIPEndPoint::OnReceivedPDU(pdu);
 }
 bool OnReceivedINVITE(SIP_PDU*request)override{if(RejectShortSipSession(*request))return false;return SIPEndPoint::OnReceivedINVITE(request);}
};
class StudioSessionRefreshWorkItem : public SIPWorkItem {
public:StudioSessionRefreshWorkItem(SIPEndPoint&ep,const PString&token):SIPWorkItem(ep,token){}
 void Work()override;
};
// Follow SIPPoolTimer's token lookup and queue grouping, without a raw connection
// pointer on the timer thread. Queued work safely expires with the connection.
class StudioSessionRefreshTimer : public PPoolTimerArg2<StudioSessionRefreshWorkItem,SIPEndPoint&,PString,SIPWorkItem> {
 using Base=PPoolTimerArg2<StudioSessionRefreshWorkItem,SIPEndPoint&,PString,SIPWorkItem>;
 PString token;
public:StudioSessionRefreshTimer(SIPThreadPool&pool,SIPEndPoint&ep,const PString&t):Base(pool,ep,t),token(t){}
 ~StudioSessionRefreshTimer(){this->m_stopped=true;}
 const char*GetGroup(const SIPWorkItem&)const override{return token;}
 PTIMER_OPERATORS(StudioSessionRefreshTimer);
};
class StudioSipConnection : public StableBfcpSipConnection {
 StudioManager& studio;
 std::atomic<bool> bfcpNegotiated{false};
 StudioSessionRefreshTimer sessionRefreshTimer;
 bool localSessionRefresher=false;
 PTimeInterval minimumSessionPeriod;
 unsigned sessionIntervalRetries=0;
 unsigned holdRequestCseq=0;
 bool creatingHoldInvite=false,unrelatedHoldResponse=false;
 HoldState holdResponseState=eHoldOff;
 struct HoldResponseGuard {
  StudioSipConnection& connection;bool previous;HoldState previousState;
  HoldResponseGuard(StudioSipConnection& c,const SIPTransaction& transaction):connection(c),previous(c.unrelatedHoldResponse),previousState(c.holdResponseState){
   c.holdResponseState=c.m_holdToRemote;
   // An earlier codec/session re-INVITE cannot acknowledge a queued hold.
   // Authentication,422 and491 retries have later CSeq values and carry the
   // still-pending hold state, so retain OPAL's normal retry/rollback handling.
   c.unrelatedHoldResponse=transaction.GetMethod()==SIP_PDU::Method_INVITE&&c.holdRequestCseq!=0&&transaction.GetMIME().GetCSeqIndex()<c.holdRequestCseq&&(c.m_holdToRemote==eHoldInProgress||c.m_holdToRemote==eRetrieveInProgress);
  }
  ~HoldResponseGuard(){connection.unrelatedHoldResponse=previous;connection.holdResponseState=previousState;}
 };
public:StudioSipConnection(const Init&i,StudioManager&s):StableBfcpSipConnection(i),studio(s),sessionRefreshTimer(i.m_endpoint.GetThreadPool(),i.m_endpoint,i.m_token){}
 void RefreshSession(){if(!IsReleased()&&localSessionRefresher)SendReINVITE(PTRACE_PARAM("RFC4028 half-interval refresh"));}
 void OnReleased()override{sessionRefreshTimer.Stop(false);SIPConnection::OnReleased();}
 void OnReceivedOK(SIPTransaction&,SIP_PDU&)override;
 bool OnHoldStateChanged(bool held)override{creatingHoldInvite=true;bool result=SIPConnection::OnHoldStateChanged(held);creatingHoldInvite=false;return result;}
 void OnHold(bool fromRemote,bool held)override{if(!fromRemote&&unrelatedHoldResponse){m_holdToRemote=holdResponseState;return;}if(!fromRemote)holdRequestCseq=0;SIPConnection::OnHold(fromRemote,held);}
 void OnTransactionFailed(SIPTransaction& transaction)override{if(IsReleased()){SIPConnection::OnTransactionFailed(transaction);return;}PSafeLockReadWrite lock(*this);if(!lock.IsLocked())return;HoldResponseGuard guard(*this,transaction);SIPConnection::OnTransactionFailed(transaction);}
 void OnReceivedINVITE(SIP_PDU&request)override{
  if(RejectShortSipSession(request))return;
  const auto&mime=request.GetMIME();auto minimum=std::max(PTimeInterval(0,90),mime.GetMinSE());
  if(mime.Has("Min-SE"))minimumSessionPeriod=std::max(minimumSessionPeriod,minimum);
  SIPConnection::OnReceivedINVITE(request);
 }
 void OnReceivedResponse(SIPTransaction&,SIP_PDU&)override;
 void AdjustInviteResponse(SIP_PDU&)override;
 void OnCreatingINVITE(SIPInvite&request)override{
  SIPConnection::OnCreatingINVITE(request);
  if(creatingHoldInvite)holdRequestCseq=request.GetMIME().GetCSeqIndex();
  if(GetPhase()>=ConnectedPhase&&m_sessionTimerPeriod>0)request.GetMIME().SetSessionExpires(m_sessionTimerPeriod,localSessionRefresher?"uac":"uas");
  if(minimumSessionPeriod>0){auto&mime=request.GetMIME();mime.SetMinSE(minimumSessionPeriod);if(mime.GetSessionExpires()<minimumSessionPeriod)mime.SetSessionExpires(minimumSessionPeriod,GetPhase()>=ConnectedPhase?(localSessionRefresher?"uac":"uas"):nullptr);}
 }
 OpalMediaFormatList GetLocalMediaFormats()override{auto formats=SIPConnection::GetLocalMediaFormats();formats+=BFCPMediaFormat::Get();return formats;}
 bool HasBfcpSession()const{return bfcpNegotiated.load();}
 OpalMediaSession*CreateMediaSession(unsigned id,const OpalMediaType&type,const PString&transport)override{
  if(type!=BFCPMediaDefinition::Name())return SIPConnection::CreateMediaSession(id,type,transport);
  auto*session=new BFCPSession(OpalMediaSession::Init(*this,id,type,m_remoteBehindNAT),this);
  if(!session->SetTransportProto(transport.IsEmpty()||transport==BFCPMediaDefinition::Name()?BFCPSession::TCP_BFCP():transport)){delete session;return nullptr;}
  session->SetStringOptions(GetStringOptions());return session;
 }
 bool OnReceivedAnswer(SIP_PDU&response,SIPTransaction*transaction)override{BfcpNegotiationTransaction negotiation(bfcpNegotiated,response.GetSDP());bool result=SIPConnection::OnReceivedAnswer(response,transaction);negotiation.Complete(result);return result;}
 bool OnSendAnswerSDP(const SDPSessionDescription&offer,SDPSessionDescription&answer,bool transfer)override{BfcpNegotiationTransaction negotiation(bfcpNegotiated,&offer);bool result=StableBfcpSipConnection::OnSendAnswerSDP(offer,answer,transfer);bfcpNegotiated=false;if(result)for(PINDEX index=1;index<=answer.GetMediaDescriptions().GetSize();++index){auto*description=answer.GetMediaDescriptionByIndex(index);if(description&&description->GetMediaType()==BFCPMediaDefinition::Name()&&description->GetPort()!=0&&description->GetMediaFormats().HasFormat(BFCPMediaFormat::Get()))bfcpNegotiated=true;}negotiation.Complete(result);return result;}
protected:bool OnSendOfferSDP(SDPSessionDescription&,bool)override;
public:
#if OPAL_BFCP
 SDPMediaDescription*OnSendAnswerSDPStream(SDPMediaDescription*,unsigned,bool,BundleMergeInfo&)override;
 bool OnReceivedAnswerSDPStream(const SDPMediaDescription*,unsigned,bool&,BundleMergeInfo&)override;
 bool OnBfcpConnected()override;
 bool OnBfcpDisconnected()override;
 bool OnFloorRequestStatusGranted(const BFCPEvent&)override;
 bool OnFloorRequestStatusAborted(const BFCPEvent&)override;
 bool OnFloorStatusGranted(const BFCPEvent&)override;
 bool OnFloorStatusAborted(const BFCPEvent&)override;
#endif
};
class StudioManager : public OpalManager {
public:
 StudioMedia media;WorkerPolicy policy;
 PcmPacing networkReceivePacing;std::atomic<int> audioPayloadMismatch{-1};
 mutable std::recursive_mutex stateMutex;std::mutex outputMutex;
 uint64_t seq=0,generation=0;PString token;std::string remote,endpoint="auto",transport="udp",encryption="disabled";
 StudioLocal*local=nullptr;StudioSip*sip=nullptr;H323EndPoint*h323=nullptr;
 bool autoAnswer=false,aacAvailable=false,connected=false,sendAudio=false,receiveAudio=false,contentWanted=false,contentActive=false,bfcpFloorGranted=false;
 bool bfcpReleasePending=false;
 SharingTransmissionMode sharingMode=SharingTransmissionMode::Auto;
 bool WantsPresentation()const{return sharingMode!=SharingTransmissionMode::MainOnly;}
 struct PendingPresentation {PString callToken;uint64_t callGeneration,contentRevision;std::string source;};
 std::optional<PendingPresentation> pendingPresentation,pendingBfcpRequest;uint64_t contentRevision=0;
 std::string desktopId,sendCodec,receiveCodec;
 Protocol actual=Protocol::Sip,mediaProtocol=Protocol::Sip,recordingProtocol=Protocol::Sip;bool recordingOrigin=false,recordingControls=false;
 telestudio::media::AudioNetworkProfile audioProfile;
#if OPAL_STATISTICS
 OpalMediaStatistics audioTxStats,audioRxStats,videoTxStats,videoRxStats;
#endif
 void UpdateOrder(){PStringArray order;for(auto&name:AudioOrder(actual==Protocol::Sip,policy.AacEnabled(),endpoint,audioProfile))order.AppendString(name.c_str());SetMediaFormatOrder(order);}
 void MediaReady(){recordingControls=!recordingOrigin&&media.recorder->Snapshot().state!=telestudio::RecordingState::Finalizing;Emit("media-ready","\"sourceControl\":true,\"muteControl\":true,\"stats\":true,\"remoteSurface\":"+std::string(media.remoteSurface?"true":"false")+",\"audioControl\":true,\"recording\":"+(recordingControls?"true":"false")+",\"systemAudioControl\":true,\"roomMixerControl\":true,\"videoControl\":true,\"holdControl\":true,\"dtmfControl\":true");}
 std::deque<std::pair<char,unsigned>>tones;std::chrono::steady_clock::time_point nextTone;std::optional<std::chrono::steady_clock::time_point>contentDeadline;
 std::optional<bool> holdRequest;std::chrono::steady_clock::time_point holdDeadline;
 void Emit(const std::string&type,const std::string&fields,Protocol protocol){std::scoped_lock lock(outputMutex);std::cout<<"{\"schema\":1,\"type\":"<<quote(type)<<",\"sequence\":"<<++seq<<",\"protocol\":"<<quote(protocol==Protocol::Sip?"sip":"h323")<<(fields.empty()?"":",")+fields<<"}\n";std::cout.flush();}
 void Emit(const std::string&type,const std::string&fields){Emit(type,fields,actual);}
 void Error(std::string code,std::string detail){Emit("media-error","\"code\":"+quote(code)+",\"detail\":"+quote(detail));}
 void Text(std::string value){std::scoped_lock lock(outputMutex);std::cout<<value<<std::endl;}
 void Availability(Protocol p){Emit("aac-availability","\"available\":"+std::string(aacAvailable?"true":"false")+",\"detail\":"+quote(aacAvailable?"AAC-LD/AAC-LC plugin loaded":"AAC plugin could not be loaded"),p);}
 void OnEstablishedCall(OpalCall&call)override {OpalManager::OnEstablishedCall(call);std::scoped_lock lock(stateMutex);if(token.IsEmpty()&&policy.Active())token=call.GetToken();if(call.GetToken()!=token)return;connected=true;auto network=Network();if(network&&media.testMode&&env("ESSASTUDIO_TEST_DTMF_INFO")=="1"&&loopback(str(network->GetRemotePartyURL())))network->SetSendUserInputMode(OpalConnection::SendUserInputAsTone);if(network)for(auto&stream:network->GetMediaStreams())if(stream.IsOpen()&&stream.GetMediaFormat().IsMediaType(OpalMediaType::Audio())){if(stream.IsSource()){receiveAudio=true;receiveCodec=str(stream.GetMediaFormat().GetName());}else{sendAudio=true;sendCodec=str(stream.GetMediaFormat().GetName());}}if(actual==Protocol::Sip)Text("call 1: CONFIRMED (connected)");else Text("call-established remote="+quote(remote));MediaReady();AudioState();AudioCodec();RoomState();if(recordingControls)RecordingState();Emit("call-hold-changed","\"held\":false");Emit("microphone-mute-changed","\"muted\":"+std::string(media.micMuted?"true":"false"));Emit("system-audio-changed","\"enabled\":"+std::string(media.systemAudio.IsRunning()?"true":"false"));if(actual==Protocol::Sip)SecurityState();}
 void OnClearedCall(OpalCall&call)override {std::scoped_lock lock(stateMutex);if(call.GetToken()==token){auto reason=str(call.GetCallEndReasonText());for(auto&c:reason)if(c==' '||c=='\r'||c=='\n')c='-';if(actual==Protocol::Sip)Text("call 1: DISCONNECTED ("+reason+")");else Text("call-cleared remote="+quote(remote)+" reason="+reason);connected=false;sendAudio=receiveAudio=false;contentActive=contentWanted=bfcpFloorGranted=bfcpReleasePending=false;pendingPresentation.reset();pendingBfcpRequest.reset();contentDeadline.reset();token=PString::Empty();tones.clear();holdRequest.reset();policy.End();media.Stop();}OpalManager::OnClearedCall(call);}
 void OnHold(OpalConnection&c,bool fromRemote,bool held)override {OpalManager::OnHold(c,fromRemote,held);std::scoped_lock lock(stateMutex);if(c.GetCall().GetToken()==token)Emit("call-hold-changed","\"held\":"+std::string(held?"true":"false"));}
 PBoolean OnOpenMediaStream(OpalConnection&c,OpalMediaStream&s)override {if(!OpalManager::OnOpenMediaStream(c,s))return false;std::scoped_lock lock(stateMutex);if(policy.Active()&&(token.IsEmpty()||c.GetCall().GetToken()==token)&&!dynamic_cast<OpalLocalConnection*>(&c)&&s.GetMediaFormat().GetMediaType()==OpalMediaType::Audio()){if(token.IsEmpty())token=c.GetCall().GetToken();if(s.IsSource()){if(auto*rtp=dynamic_cast<OpalRTPMediaStream*>(&s)){OpalJitterBuffer::Init jitter(c.GetJitterParameters(),s.GetMediaFormat().GetTimeUnits(),GetMaxRtpPayloadSize());jitter.m_currentJitterDelay=dynamic_cast<H323Connection*>(&c)?audioProfile.jitterMinimumMs:audioProfile.jitterInitialMs;rtp->SetJitterBufferDelay(jitter);}receiveAudio=true;receiveCodec=str(s.GetMediaFormat().GetName());}else{sendAudio=true;sendCodec=str(s.GetMediaFormat().GetName());}if(connected)AudioCodec();}return true;}
 void OnUserInputString(OpalConnection&c,const PString&value)override {OpalManager::OnUserInputString(c,value);std::scoped_lock lock(stateMutex);if(c.GetCall().GetToken()==token&&!dynamic_cast<OpalLocalConnection*>(&c))Emit("opal-dtmf-received","\"digits\":"+quote(str(value)),dynamic_cast<H323Connection*>(&c)?Protocol::H323:Protocol::Sip);}
 void OnUserInputTone(OpalConnection&c,char tone,int duration)override {OpalManager::OnUserInputTone(c,tone,duration);std::scoped_lock lock(stateMutex);if(duration>0&&c.GetCall().GetToken()==token&&!dynamic_cast<OpalLocalConnection*>(&c))Emit("opal-dtmf-received","\"digits\":"+quote(std::string(1,tone))+",\"durationMs\":"+std::to_string(duration),dynamic_cast<H323Connection*>(&c)?Protocol::H323:Protocol::Sip);}
 void OnClosedMediaStream(const OpalMediaStream&s)override {OpalManager::OnClosedMediaStream(s);std::scoped_lock lock(stateMutex);auto&c=s.GetConnection();if(c.GetCall().GetToken()!=token||dynamic_cast<OpalLocalConnection*>(&c)||!s.GetMediaFormat().IsMediaType(OpalMediaType::Audio()))return;bool active=false;for(auto&stream:c.GetMediaStreams())if(stream.IsOpen()&&stream.IsSource()==s.IsSource()&&stream.GetMediaFormat().IsMediaType(OpalMediaType::Audio()))active=true;if(s.IsSource())receiveAudio=active;else sendAudio=active;if(connected)AudioCodec();}
 void AdjustMediaFormats(bool localMedia,const OpalConnection&c,OpalMediaFormatList&formats)const override {
  OpalManager::AdjustMediaFormats(localMedia,c,formats);std::scoped_lock lock(stateMutex);OpalMediaFormatList removed;
  bool sipProtocol=dynamic_cast<const SIPConnection*>(&c)!=nullptr||(dynamic_cast<const OpalLocalConnection*>(&c)!=nullptr&&actual==Protocol::Sip);
  for(auto&format:formats){auto name=str(format.GetName());bool raw=!format.IsTransportable(),aac=name.starts_with("MyStudio-AAC-");format.SetOptionString(OpalMediaFormat::ProtocolOption(),sipProtocol?"SIP":"H.323");
   bool keep=raw||name==str(OpalG722.GetName())||name==str(OpalH264.GetName())||(sipProtocol&&name==str(OpalH264_MODE0.GetName()))||name==str(OpalRFC2833.GetName());
#if OPAL_BFCP
   keep|=name==str(BFCPMediaFormat::Get().GetName());
#endif
   if(aac)keep=AllowsAac(name,(dynamic_cast<const SIPConnection*>(&c)!=nullptr||(dynamic_cast<const OpalLocalConnection*>(&c)!=nullptr&&actual==Protocol::Sip)),policy.AacEnabled(),endpoint,audioProfile);
   if(!keep)removed+=format;
   if(localMedia&&format.GetMediaType()==OpalMediaType::Video())ConfigureVideoFormat(format,media.settings.width,media.settings.height,media.bitrateKbps,media.fps);
  } formats-=removed;PStringArray order;for(auto&name:AudioOrder((dynamic_cast<const SIPConnection*>(&c)!=nullptr||(dynamic_cast<const OpalLocalConnection*>(&c)!=nullptr&&actual==Protocol::Sip)),policy.AacEnabled(),endpoint,audioProfile))order.AppendString(name.c_str());formats.Reorder(order);
 }
 bool OnChangedPresentationRole(OpalConnection&c,const PString&chair,bool request)override {if(!OpalManager::OnChangedPresentationRole(c,chair,request))return false;std::scoped_lock lock(stateMutex);if(c.GetCall().GetToken()!=token||dynamic_cast<OpalLocalConnection*>(&c))return true;bool owned=chair==c.GetLocalPartyURL();if(owned&&contentWanted&&WantsPresentation()){auto localConnection=local->GetLocalConnectionWithLock(token,PSafeReadWrite);if(localConnection&&OpenPresentation(c,*localConnection)){contentActive=true;contentDeadline.reset();media.Desktop(true,desktopId,true);Emit("content-sharing-state","\"mode\":\"h239\",\"active\":true,\"detail\":\"Presentation token granted and independent video channel opened\"");}else ContentFallback("H.239 presentation channel could not open");}else if(contentActive){contentActive=false;media.Desktop(false,{},true);Emit("content-sharing-state","\"mode\":\"h239\",\"active\":false,\"detail\":\"Presentation token released\"");}return true;}
 void EchoState(){Emit("echo-cancellation-state","\"available\":"+std::string(media.EchoAvailable()?"true":"false")+",\"enabled\":"+(media.EchoEnabled()?"true":"false"));}
 void AudioState(){auto s=media.audio.ConfirmedState();Emit("audio-control-changed","\"roomVolume\":"+std::to_string(s.roomVolumePercent)+",\"micGain\":"+std::to_string(s.microphoneGainPercent)+",\"noiseIsolation\":"+(s.noiseIsolation?"true":"false"));EchoState();}
 void RoomState(){auto s=media.audio.ConfirmedRoomMixerState();Emit("room-mixer-changed","\"masterPercent\":"+std::to_string(s.masterPercent)+",\"bassDb\":"+std::to_string(s.bassDb)+",\"voiceDb\":"+std::to_string(s.voiceDb)+",\"trebleDb\":"+std::to_string(s.trebleDb)+",\"noiseReductionDb\":"+std::to_string(s.noiseReductionDb));}
 void AudioCodec(){if(sendCodec.empty()&&receiveCodec.empty())return;auto details=[](const std::string&name){bool ld=name.find("AAC-LD")!=std::string::npos,lc=name.find("AAC-LC")!=std::string::npos;std::string codec=ld?"aac-ld":lc?"aac-lc":"g722";int rate=ld?48000:lc?(name.find("32k")!=std::string::npos?32000:48000):16000,channels=name.find("stereo")!=std::string::npos||name.ends_with("128")?2:1,bitrate=lc?(channels==2?192:96):name.ends_with("128")?128:64;return std::tuple{codec,rate,channels,bitrate};};
  auto [codec,rate,channels,bitrate]=details(!sendCodec.empty()?sendCodec:receiveCodec);auto requested=policy.AacEnabled()?((endpoint=="pcs-xg77"||endpoint=="pcs-xg80")?"aac-lc":"aac-ld"):"g722";
  Emit("audio-codec-state","\"requested\":"+quote(requested)+",\"negotiated\":"+quote(codec)+",\"sampleRate\":"+std::to_string(rate)+",\"channels\":"+std::to_string(channels)+",\"bitrateKbps\":"+std::to_string(bitrate)+",\"sendActive\":"+(sendAudio?"true":"false")+",\"receiveActive\":"+(receiveAudio?"true":"false"));
  for(bool send:{true,false}){auto name=send?sendCodec:receiveCodec;if(name.empty())continue;auto [directionCodec,directionRate,directionChannels,directionBitrate]=details(name);Emit("audio-direction-state","\"direction\":"+quote(send?"send":"receive")+",\"codec\":"+quote(directionCodec)+",\"sampleRate\":"+std::to_string(directionRate)+",\"channels\":"+std::to_string(directionChannels)+",\"bitrateKbps\":"+std::to_string(directionBitrate));}
 }
 void SecurityState(){auto connection=Network();std::string observed="unknown",security="unknown";if(connection){observed=str(connection->GetRemoteAddress().GetProtoPrefix());if(!observed.empty()&&observed.back()=='$')observed.pop_back();if(observed!="udp"&&observed!="tcp"&&observed!="tls")observed="unknown";auto*rtp=dynamic_cast<OpalRTPConnection*>(&*connection);auto*session=rtp?rtp->FindSessionByMediaType(OpalMediaType::Audio()):nullptr;if(session)security=session->IsCryptoSecured(true)&&session->IsCryptoSecured(false)?"srtp":"none";}Emit("sip-security-state","\"transport\":"+quote(observed)+",\"mediaEncryption\":"+quote(security));}
 void RecordingState(){auto s=media.RecordingStatus();if(connected&&!recordingOrigin&&(s.state==telestudio::RecordingState::Completed||s.state==telestudio::RecordingState::Failed))s={};static const char*states[]={"idle","starting","recording","finalizing","completed","failed"};Emit("recording-state","\"state\":"+quote(states[int(s.state)])+",\"micIncluded\":"+(s.micIncluded?"true":"false")+",\"roomIncluded\":"+(s.roomIncluded?"true":"false")+",\"elapsedMs\":"+std::to_string(s.elapsedMs)+",\"fileName\":"+quote(s.fileName)+",\"paused\":"+(s.paused?"true":"false"),recordingOrigin?recordingProtocol:actual);}
 void RecordingError(std::string code,std::string detail,bool request=false){Emit("recording-error","\"code\":"+quote(code)+",\"detail\":"+quote(detail)+",\"recoverablePart\":false,\"fileName\":\"\"",recordingOrigin&&!request?recordingProtocol:actual);}
 void Failure(std::string code,std::string detail,std::string target){Error(code,detail);media.Stop();policy.End();connected=false;token=PString::Empty();if(actual==Protocol::Sip)Text("startup error: "+code+": "+detail);else Text("dial-error reason="+code);}
 void PumpHold(){
  PString callToken;bool wanted;{std::scoped_lock lock(stateMutex);if(!holdRequest)return;if(!connected||token.IsEmpty()){holdRequest.reset();return;}callToken=token;wanted=*holdRequest;}
  auto call=FindCallWithLock(callToken,PSafeReference);bool negotiating=false;
  if(call)for(PINDEX i=0;i<call->GetConnectionCount();++i){auto c=call->GetConnection(i,PSafeReference);auto*h=c?dynamic_cast<H323Connection*>(&*c):nullptr;if(h&&h->IsSendingCapabilitySet())negotiating=true;}
  if(negotiating){std::scoped_lock lock(stateMutex);if(token==callToken&&std::chrono::steady_clock::now()>=holdDeadline){holdRequest.reset();Error("hold-rejected","H.323 capability exchange did not complete");}return;}
  {std::scoped_lock lock(stateMutex);if(token!=callToken||!holdRequest)return;holdRequest.reset();}
  bool accepted=call&&call->Hold(wanted);call.SetNULL();if(!accepted){std::scoped_lock lock(stateMutex);if(token==callToken)Error("hold-rejected","Unable to change call hold");}
 }
 void PumpDtmf(){std::scoped_lock lock(stateMutex);if(tones.empty()||std::chrono::steady_clock::now()<nextTone)return;auto tone=tones.front();tones.pop_front();auto c=Network();if(!c||!c->SendUserInputTone(tone.first,tone.second)){tones.clear();Error("dtmf-unavailable","OPAL could not send DTMF");}nextTone=std::chrono::steady_clock::now()+std::chrono::milliseconds(tone.second+220);}

 PSafePtr<OpalConnection> Network(){auto call=FindCallWithLock(token,PSafeReference);if(call)for(PINDEX i=0;i<call->GetConnectionCount();++i){auto c=call->GetConnection(i,PSafeReference);if(c&&!dynamic_cast<OpalLocalConnection*>(&*c))return c;}return {};}
 bool BfcpRequest(const PendingPresentation& request,bool release,std::string*failure=nullptr){
#if OPAL_BFCP
  auto call=FindCallWithLock(request.callToken,PSafeReference);PSafePtr<OpalConnection> connection;
  if(call)for(PINDEX i=0;i<call->GetConnectionCount();++i){auto candidate=call->GetConnection(i,PSafeReference);if(candidate&&!dynamic_cast<OpalLocalConnection*>(&*candidate)){connection=candidate;break;}}
  // SIP negotiation owns this same connection lock. Acquire it without the
  // application state lock, then validate the captured command before use.
  if(!connection||!connection.SetSafetyMode(PSafeReadWrite)){if(failure)*failure="BFCP call is no longer available";return false;}
  {std::scoped_lock lock(stateMutex);if(request.callToken!=token||request.callGeneration!=generation||request.contentRevision!=contentRevision||contentWanted==release){if(failure)*failure="BFCP request was superseded";return false;}if(!release&&(bfcpReleasePending||bfcpFloorGranted||contentActive))return true;}
  auto*rtp=dynamic_cast<OpalRTPConnection*>(&*connection);
  auto*session=rtp?dynamic_cast<BFCPSession*>(rtp->FindSessionByMediaType(BFCPMediaDefinition::Name())):nullptr;
  auto*sipConnection=connection?dynamic_cast<StudioSipConnection*>(&*connection):nullptr;
  if(!session){if(failure)*failure="BFCP control session is missing";return false;}
  if(!release&&(!sipConnection||!sipConnection->HasBfcpSession())){if(failure)*failure="BFCP was not accepted in the current SDP negotiation";return false;}
  if(!session->RequestFloor(release)){if(failure)*failure=!session->IsOpen()?"Negotiated BFCP transport is disconnected":release?"BFCP floor release request was rejected":"BFCP floor request was rejected by the control state";return false;}
  return true;
#else
  if(failure)*failure="BFCP is unavailable in this build";
  return false;
#endif
 }
 void PumpBfcpRequest(){
  std::optional<PendingPresentation> request;
  {std::scoped_lock lock(stateMutex);if(!pendingBfcpRequest)return;request=std::move(pendingBfcpRequest);pendingBfcpRequest.reset();if(contentActive||bfcpReleasePending||bfcpFloorGranted||!connected||!contentWanted||!WantsPresentation()||request->callToken!=token||request->callGeneration!=generation||request->contentRevision!=contentRevision)return;}
  std::string failure;const bool accepted=BfcpRequest(*request,false,&failure);
  {std::scoped_lock lock(stateMutex);if(!connected||!contentWanted||request->callToken!=token||request->callGeneration!=generation||request->contentRevision!=contentRevision)return;if(!accepted&&!contentActive&&!bfcpFloorGranted)ContentFallback(failure);else if(!contentActive&&!bfcpFloorGranted)contentDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);}
 }
 bool OpenPresentation(OpalConnection&network,OpalLocalConnection&capture){
  // H264 formats retain media type video; presentation is a content role/session.
  // Preserve the peer's role-mask/options rather than replacing with a global format.
  unsigned sessionId=0;
  if(auto*rtp=dynamic_cast<OpalRTPConnection*>(&network)){
   if(auto*session=rtp->FindSessionByMediaType(OpalPresentationVideoMediaDefinition::Name()))sessionId=session->GetSessionID();
   else if(auto*bfcp=dynamic_cast<BFCPSession*>(rtp->FindSessionByMediaType(BFCPMediaDefinition::Name())))for(auto&floor:bfcp->GetFloorStreamMap()){auto*session=rtp->GetMediaSession(floor.second);if(session&&session->GetVideoContentRole()==OpalVideoFormat::ePresentation){sessionId=session->GetSessionID();break;}}
  }
  if(dynamic_cast<H323Connection*>(&network)){for(auto&stream:network.GetMediaStreams())if(stream.IsOpen()&&!stream.IsSource()&&stream.GetMediaFormat().GetOptionEnum(OpalVideoFormat::ContentRoleOption(),OpalVideoFormat::eNoRole)==OpalVideoFormat::ePresentation){sessionId=stream.GetSessionID();break;}if(!sessionId){sessionId=H323Capability::MasterAllocatedBaseSessionID;while(network.GetMediaStream(sessionId,true)||network.GetMediaStream(sessionId,false)||capture.GetMediaStream(sessionId,true))++sessionId;}}
  if(!sessionId)return false;
  if(!network.GetCall().OpenSourceMediaStreams(capture,OpalMediaType::Video(),sessionId,OpalMediaFormat(),OpalVideoFormat::ePresentation))return false;
  auto stream=network.GetMediaStream(sessionId,false);
  return stream&&stream->IsOpen()&&stream->GetMediaFormat().GetOptionEnum(OpalVideoFormat::ContentRoleOption(),OpalVideoFormat::eNoRole)==OpalVideoFormat::ePresentation;
 }
 void BfcpGranted(OpalConnection&connection,bool granted){std::scoped_lock lock(stateMutex);if(connection.GetCall().GetToken()!=token)return;
  if(granted&&bfcpReleasePending)return;
  const bool completedRelease=bfcpReleasePending&&!granted;
  if(!granted)bfcpReleasePending=false;
  bfcpFloorGranted=granted;pendingPresentation.reset();if(granted)pendingBfcpRequest.reset();
  if(completedRelease&&contentWanted&&WantsPresentation()&&connected){
   pendingBfcpRequest=PendingPresentation{token,generation,contentRevision,desktopId};
   contentDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
   return;
  }
  if(granted&&contentWanted&&WantsPresentation()&&connected&&dynamic_cast<StudioSipConnection*>(&connection)&&static_cast<StudioSipConnection&>(connection).HasBfcpSession()){
   // Opening a SIP stream may renegotiate and invoke manager callbacks. Queue it
   // outside the BFCP transport callback and the application state lock.
   pendingPresentation=PendingPresentation{token,generation,contentRevision,desktopId};contentDeadline.reset();
  }else if(contentWanted&&connected){ContentFallback(granted?"BFCP grant lacks an accepted presentation session":"Presentation floor released or revoked; main sharing retained");}
  else if(contentActive){contentActive=false;media.Desktop(false,{},true);Emit("content-sharing-state","\"mode\":\"bfcp\",\"active\":false,\"detail\":\"Presentation floor released or revoked\"");}
 }
 void PumpPresentation(){
  std::optional<PendingPresentation> request;{std::scoped_lock lock(stateMutex);if(!pendingPresentation)return;request=std::move(pendingPresentation);pendingPresentation.reset();if(!connected||!contentWanted||!WantsPresentation()||!bfcpFloorGranted||request->callToken!=token||request->callGeneration!=generation||request->contentRevision!=contentRevision)return;}
  auto call=FindCallWithLock(request->callToken,PSafeReference);PSafePtr<OpalConnection> network;
  if(call)for(PINDEX i=0;i<call->GetConnectionCount();++i){auto c=call->GetConnection(i,PSafeReference);if(c&&!dynamic_cast<OpalLocalConnection*>(&*c)){network=c;break;}}
  auto capture=local->GetLocalConnectionWithLock(request->callToken,PSafeReference);
  bool opened=false;
  if(network&&capture&&network.SetSafetyMode(PSafeReadWrite)){
   bool current=false;{std::scoped_lock lock(stateMutex);current=connected&&contentWanted&&bfcpFloorGranted&&request->callToken==token&&request->callGeneration==generation&&request->contentRevision==contentRevision;}
   if(current)opened=OpenPresentation(*network,*capture);
  }
  network.SetNULL();capture.SetNULL();call.SetNULL();
  {std::scoped_lock lock(stateMutex);if(!connected||!contentWanted||!WantsPresentation()||!bfcpFloorGranted||request->callToken!=token||request->callGeneration!=generation||request->contentRevision!=contentRevision||request->source!=desktopId)return;
   if(opened){contentActive=true;media.Desktop(true,request->source,true);Emit("content-sharing-state","\"mode\":\"bfcp\",\"active\":true,\"detail\":\"BFCP floor granted; main and dedicated presentation video channels enabled\"");}
   else ContentFallback("BFCP granted but presentation video could not open");
  }
 }
 PDECLARE_NOTIFIER(RTP_DataFrame,StudioManager,AacMarker);
 PDECLARE_NOTIFIER(RTP_DataFrame,StudioManager,IncomingAudio);
 void OnStartMediaPatch(OpalConnection&connection,OpalMediaPatch&patch)override{
  OpalManager::OnStartMediaPatch(connection,patch);
  if(&connection!=&patch.GetSource().GetConnection())return;
  if(!dynamic_cast<OpalLocalConnection*>(&connection)&&patch.GetSource().GetMediaFormat().IsMediaType(OpalMediaType::Audio())){auto format=patch.GetSource().GetMediaFormat();PTRACE(3,"OPAL incoming audio filter "<<format<<" clock="<<format.GetClockRate()<<" frame="<<format.GetFrameTime()<<" payload="<<format.GetPayloadType());patch.AddFilter(PCREATE_NOTIFIER(IncomingAudio),format);}
  for(auto name:{"MyStudio-AAC-LD-LATM-64","MyStudio-AAC-LD-LATM-128","MyStudio-AAC-LC-48k-stereo192","MyStudio-AAC-LC-48k-mono96","MyStudio-AAC-LC-32k-stereo192","MyStudio-AAC-LC-32k-mono96"}){OpalMediaFormat format(name);if(format.IsValid())patch.AddFilter(PCREATE_NOTIFIER(AacMarker),format);}
 }
 void ContentFallback(std::string reason){pendingPresentation.reset();contentDeadline.reset();contentActive=false;media.Desktop(contentWanted,desktopId,false);Emit("content-sharing-state","\"mode\":\"main-video\",\"active\":false,\"detail\":"+quote(reason+"; composed main video requested"));}
 void Desktop(bool on,std::string id,std::unique_lock<std::recursive_mutex>&stateLock){++contentRevision;pendingPresentation.reset();pendingBfcpRequest.reset();contentWanted=on;contentDeadline.reset();if(on)desktopId=std::move(id);auto c=Network();
  if(on&&!WantsPresentation()){media.Desktop(false,{},true);ContentFallback("Main-only sharing selected");return;}
  if(on&&contentActive){media.Desktop(true,desktopId,true);Emit("content-sharing-state","\"mode\":"+quote(actual==Protocol::Sip?"bfcp":"h239")+",\"active\":true,\"detail\":\"Existing granted presentation channel reused\"");return;}
  if(on)media.Desktop(true,desktopId,false);
  if(actual==Protocol::Sip){if(!on)bfcpReleasePending=true;const PendingPresentation request{token,generation,contentRevision,desktopId};std::string failure;stateLock.unlock();bool accepted=false;try{accepted=BfcpRequest(request,!on,&failure);}catch(...){stateLock.lock();throw;}stateLock.lock();if(request.callToken!=token||request.callGeneration!=generation||request.contentRevision!=contentRevision)return;
   if(!on){if(!accepted)bfcpReleasePending=false;media.Desktop(false,{},true);media.Desktop(false);contentActive=false;Emit("content-sharing-state","\"mode\":\"bfcp\",\"active\":false,\"detail\":"+quote(accepted?"Sharing stopped; floor release requested":"Sharing stopped; "+failure));return;}
   if(accepted||bfcpFloorGranted||contentActive){if(bfcpFloorGranted&&!bfcpReleasePending&&!contentActive)pendingPresentation=request;if(bfcpReleasePending||(!bfcpFloorGranted&&!contentActive))contentDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);Emit("content-sharing-state","\"mode\":\"bfcp\",\"active\":false,\"detail\":"+quote(bfcpFloorGranted?"BFCP floor granted; opening presentation":"Awaiting BFCP floor grant"));return;}
   ContentFallback(failure);return;
  }
  if(c){if(!on){c->RequestPresentationRole(true);media.Desktop(false,{},true);media.Desktop(false);contentActive=false;Emit("content-sharing-state","\"mode\":\"h239\",\"active\":false,\"detail\":\"Sharing stopped\"");return;}if(c->RequestPresentationRole(false)){contentDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);Emit("content-sharing-state","\"mode\":\"h239\",\"active\":false,\"detail\":\"Awaiting presentation token\"");return;}}
  ContentFallback("Peer did not accept H.239 token request");
 }
 void PumpContent(){std::scoped_lock lock(stateMutex);if(connected&&contentWanted&&!contentActive&&contentDeadline&&std::chrono::steady_clock::now()>=*contentDeadline)ContentFallback("Presentation floor/token grant timed out");}
 std::chrono::steady_clock::time_point nextVideoDiagnostics{};
 void VideoDiagnostics(){
  PSafePtr<OpalConnection> connection;PString callToken;uint64_t callGeneration;Protocol protocol;
  {std::scoped_lock lock(stateMutex);auto now=std::chrono::steady_clock::now();if(!connected||now<nextVideoDiagnostics)return;nextVideoDiagnostics=now+std::chrono::seconds(10);connection=Network();callToken=token;callGeneration=generation;protocol=actual;}
  // Never wait for a connection lock while holding the lifecycle mutex.
  if(!connection||!connection.SetSafetyMode(PSafeReadOnly))return;
  std::vector<std::string> records;auto*rtp=dynamic_cast<OpalRTPConnection*>(&*connection);
  if(rtp)for(auto&stream:connection->GetMediaStreams()){
   auto format=stream.GetMediaFormat();if(!stream.IsOpen()||!format.IsMediaType(OpalMediaType::Video())||records.size()>=8)continue;
   auto*session=dynamic_cast<OpalRTPSession*>(rtp->GetMediaSession(stream.GetSessionID()));if(!session)continue;
   int mode=format.GetOptionInteger(PLUGINCODEC_OPTION_H264_PACKET_MODE,-1),lost=session->GetPacketsLostByRemote();
   PString profile;format.GetOptionValue("SIP/SDP Profile & Level",profile);
   uint64_t receivedPackets=0,receivedOctets=0;for(auto ssrc:session->GetSyncSources(OpalRTPSession::e_Receiver)){receivedPackets+=session->GetPacketsReceived(ssrc);receivedOctets+=session->GetOctetsReceived(ssrc);}
   auto role=session->GetVideoContentRole();std::string roleName=role==OpalVideoFormat::ePresentation?"presentation":role==OpalVideoFormat::eMainRole?"main":"unknown";
   records.push_back("\"sessionId\":"+std::to_string(stream.GetSessionID())+",\"direction\":"+quote(stream.IsSource()?"receive":"send")+",\"role\":"+quote(roleName)+",\"codec\":"+quote(str(format.GetName()))+
    ",\"payloadType\":"+std::to_string(format.GetPayloadType())+",\"width\":"+std::to_string(format.GetOptionInteger(OpalVideoFormat::FrameWidthOption(),0))+",\"height\":"+std::to_string(format.GetOptionInteger(OpalVideoFormat::FrameHeightOption(),0))+
    ",\"packetizationMode\":"+(mode<0?"null":std::to_string(mode))+",\"profileLevelId\":"+quote(str(profile))+
    ",\"localRtp\":"+quote(str(session->GetLocalAddress()))+",\"remoteRtp\":"+quote(str(session->GetRemoteAddress()))+
    ",\"packetsSent\":"+std::to_string(session->GetPacketsSent())+",\"octetsSent\":"+std::to_string(session->GetOctetsSent())+",\"markersSent\":"+std::to_string(session->GetMarkerSendCount())+
    ",\"packetsReceived\":"+std::to_string(receivedPackets)+",\"octetsReceived\":"+std::to_string(receivedOctets)+",\"remotePacketsLost\":"+(lost==-1?"null":std::to_string(lost))+",\"senderSsrc\":"+std::to_string(session->GetSyncSourceOut()));
  }
  connection.SetNULL();
  {std::scoped_lock lock(stateMutex);if(!connected||callToken!=token||callGeneration!=generation)return;for(auto&record:records)Emit("opal-video-transport",record,protocol);}
 }
 void Diagnostics(){std::scoped_lock lock(stateMutex);if(connected){int mismatched=audioPayloadMismatch.exchange(-1);if(mismatched>=0)Error("audio-payload-mismatch","Dropped RTP payload type "+std::to_string(mismatched)+" which differs from the negotiated audio codec");}if(!policy.Active())return;std::string audioTx="null",audioRx="null",videoTx="null",videoRx="null",loss="null",jitter="null",buffer="null",rtt="null";
#if OPAL_STATISTICS
  auto connection=Network();if(connection){auto rate=[&](OpalMediaStatistics&stats,const OpalMediaType&type,bool source){auto*rtp=dynamic_cast<OpalRTPConnection*>(&*connection);auto*session=rtp?rtp->FindSessionByMediaType(type):nullptr;auto stream=session?connection->GetMediaStream(session->GetSessionID(),source):OpalMediaStreamPtr();if(!stream||!stream->IsOpen())return std::string("null");stats.Update(*stream);return stats.IsValid()?std::to_string(stats.GetBitRate()/1000.0):std::string("null");};audioTx=rate(audioTxStats,OpalMediaType::Audio(),false);audioRx=rate(audioRxStats,OpalMediaType::Audio(),true);videoTx=rate(videoTxStats,OpalMediaType::Video(),false);videoRx=rate(videoRxStats,OpalMediaType::Video(),true);auto number=[](int v){return v<0?std::string("null"):std::to_string(v);};jitter=number(audioRxStats.m_averageJitter);buffer=number(audioRxStats.m_jitterBufferDelay);rtt=number(audioTxStats.m_roundTripTime);if(audioRxStats.m_packetsLost>=0&&audioRxStats.m_totalPackets)loss=std::to_string(100.0*audioRxStats.m_packetsLost/(audioRxStats.m_totalPackets+audioRxStats.m_packetsLost));}
#endif
  auto meters=media.audio.RoomMeters();Emit("room-audio-meter","\"preLimiterPeakQ15\":"+std::to_string(meters.preLimiterPeakQ15)+",\"postLimiterPeakQ15\":"+std::to_string(meters.postLimiterPeakQ15)+",\"limiterGainReductionQ15\":"+std::to_string(meters.gainReductionQ15)+",\"limiterActive\":"+(meters.limiterActive?"true":"false"));
  uint64_t capture,decoded,nonzero,submitted;{std::scoped_lock audioLock(media.audioMutex);capture=media.captureSamples;decoded=media.playbackSamples;nonzero=media.decodedNonzeroSamples;submitted=media.deviceSubmittedNonzeroSamples;}
  std::string rates="\"audioTxKbps\":"+audioTx+",\"audioRxKbps\":"+audioRx+",\"videoTxKbps\":"+videoTx+",\"videoRxKbps\":"+videoRx;Emit("opal-connection-health",rates+",\"lossPercent\":"+loss+",\"jitterBufferMs\":"+buffer+",\"roundTripMs\":"+rtt+",\"capturedSamples\":"+std::to_string(capture)+",\"decodedAudioSamples\":"+std::to_string(decoded)+",\"sentFrames\":"+std::to_string(media.sentFrames.load())+",\"receivedFrames\":"+std::to_string(media.receivedFrames.load())+",\"decodedNonzeroSamples\":"+std::to_string(nonzero)+",\"deviceSubmittedNonzeroSamples\":"+std::to_string(submitted));Emit("opal-content-health","\"sentFrames\":"+std::to_string(media.sentContentFrames.load())+",\"receivedFrames\":"+std::to_string(media.receivedContentFrames.load()));Emit("transport-stats","\"lossPercent\":"+loss+",\"jitterMs\":"+jitter+",\"sendQueueMs\":null,"+rates);}
 void Command(std::string line){std::unique_lock lock(stateMutex);
  if(line.starts_with("protocol ")){if(!policy.Select(line.substr(9)))Error("protocol-change-rejected","Protocol can change while idle only");else{actual=policy.Selected();UpdateOrder();}return;}
  if(line.starts_with("aac-enabled ")){if(!policy.ConfigureAac(line.substr(12),aacAvailable))Error("aac-change-rejected","AAC requires idle state and a loaded plugin");else UpdateOrder();Availability(actual);return;}
  if(line.starts_with("audio-devices ")){std::istringstream in(line.substr(14));std::string microphone,speaker,extra;in>>microphone>>speaker;if(policy.Active()||microphone.empty()||speaker.empty()||in>>extra){Error("audio-devices-rejected","Audio devices can change while idle only");return;}auto mic=microphone=="default"?"":decode(microphone),spk=speaker=="default"?"":decode(speaker);_putenv_s("ESSASTUDIO_MICROPHONE_ID",mic.c_str());_putenv_s("ESSASTUDIO_SPEAKER_ID",spk.c_str());return;}
  if(line=="hold"||line=="hold on"||line=="hold off"||line=="resume"){if(!connected){Error("hold-rejected","No connected call");return;}holdRequest=line!="hold off"&&line!="resume";holdDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);return;}
  if(line.starts_with("dtmf ")){std::istringstream in(line.substr(5));std::string digits,extra,durationText;unsigned duration=160;in>>digits;bool invalidDuration=(in>>durationText)&&!telestudio::media::ProfileNumber(durationText,duration);if(invalidDuration||in>>extra||digits.empty()||digits.size()>32||digits.find_first_not_of("0123456789*#ABCD")!=std::string::npos||duration<80||duration>2000){Error("dtmf-invalid","Invalid DTMF sequence");return;}if(!connected){Error("dtmf-unavailable","No connected call");return;}if(tones.size()+digits.size()>32){Error("dtmf-busy","DTMF queue is full");return;}for(char d:digits)tones.emplace_back(d,duration);return;}

  if(line.starts_with("surface-")||line.starts_with("local-surface-")){std::istringstream in(line);std::string action;uint64_t epoch=0;in>>action>>epoch;bool isLocal=action.starts_with("local-");auto kind=action.find("opened")!=std::string::npos?telestudio::SurfaceAcknowledgement::Opened:action.find("released")!=std::string::npos?telestudio::SurfaceAcknowledgement::Released:telestudio::SurfaceAcknowledgement::Removed;media.Ack(isLocal,kind,epoch);return;}
  if(line.starts_with("record start ")){std::istringstream args(line);std::string record,start,id,mic,room,width,height,extra;args>>record>>start>>id>>mic>>room;if(args>>width){if(!(args>>height)||width!="1920"||height!="1080"||args>>extra){RecordingError("recording-resolution-rejected","OPAL recording requires1920x1080",true);return;}line="record start "+id+" "+mic+" "+room;}}
  auto c=ParseSipCommand(line);switch(c.kind){
   case SipCommandKind::Empty:case SipCommandKind::Quit:return;
   case SipCommandKind::Call:{if(policy.Active()){Error("call-busy","Only one active call is allowed");break;}std::string target=c.argument;if(!target.starts_with("sip:")&&!target.starts_with("sips:")&&!target.starts_with("h323:"))target=(actual==Protocol::Sip?"sip:":"h323:")+target;
    if(autoAnswer&&!loopback(target)){Failure("test-loopback-only","Test worker permits loopback calls only",target);break;}
    if((actual==Protocol::Sip&&target.starts_with("h323:"))||(actual==Protocol::H323&&!target.starts_with("h323:"))){Failure("protocol-mismatch","Target scheme differs from selected protocol",target);break;}
    if(actual==Protocol::Sip){auto requested=target.starts_with("sips:")?"tls":transport=="tls"?"tls":c.transport=="tcp"?"tcp":transport;
#if !OPAL_PTLIB_SSL
     if(requested=="tls"){Failure("tls-unavailable","TLS is not compiled in this OPAL build",target);break;}
#endif
     SIPURL uri(target.c_str());uri.SetTransport(requested.c_str());target=str(uri.AsString());}

    try{policy.Begin(actual);UpdateOrder();remote=target;connected=false;sendAudio=receiveAudio=false;sendCodec.clear();receiveCodec.clear();mediaProtocol=actual;media.Start(++generation);PString created;
     if(actual==Protocol::Sip)Text("call 1: CALLING (dialing)");else Text("dial-start destination="+quote(remote)+" gatekeeper=disabled");
     lock.unlock();bool setup=false;try{setup=SetUpCall("local:*",target.c_str(),created);}catch(...){lock.lock();throw;}lock.lock();
     if(!setup){Failure("call-setup-failed","OPAL could not set up call",target);break;}
     if(policy.Active())token=created;
    }catch(const std::exception&e){Failure("call-setup-failed",e.what(),target);}break;}

   case SipCommandKind::Answer:{auto incoming=token;lock.unlock();bool accepted=local->AcceptIncomingCall(incoming);lock.lock();if(!accepted)Error("answer-rejected","No incoming call to answer");break;}
   case SipCommandKind::Reject:{auto incoming=token;lock.unlock();bool accepted=local->RejectIncomingCall(incoming);lock.lock();if(!accepted)Error("reject-rejected","No incoming call to reject");break;}
   case SipCommandKind::HangUp:{auto ending=token;lock.unlock();if(!ending.IsEmpty())ClearCall(ending);break;}
   case SipCommandKind::Mute:case SipCommandKind::MicrophoneMute:media.micMuted=c.enabled;media.mixer->SetMicrophoneMuted(c.enabled);Emit("microphone-mute-changed","\"muted\":"+std::string(c.enabled?"true":"false"));Emit("mute-changed","\"muted\":"+std::string(c.enabled?"true":"false"));break;
   case SipCommandKind::SystemAudio:if(!media.SystemAudio(c.enabled))Error("system-audio-unavailable",media.systemAudio.LastError());break;
   case SipCommandKind::RoomMute:{auto s=media.audio.ConfirmedRoomMixerState();s.muted=c.enabled;media.audio.TrySetRoomMixer(s);Emit("room-audio-mute-changed","\"muted\":"+std::string(s.muted?"true":"false"));RoomState();break;}
   case SipCommandKind::RoomMixer:{auto old=media.audio.ConfirmedRoomMixerState();if(!media.audio.TrySetRoomMixer({c.roomMixer.masterPercent,c.roomMixer.bassDb,c.roomMixer.voiceDb,c.roomMixer.trebleDb,old.muted,c.roomMixer.noiseReductionDb}))Error("room-mixer-invalid","Mixer settings unavailable");RoomState();break;}
   case SipCommandKind::RoomVolume:if(!media.audio.TrySetRoomVolume(c.integerArgument))Error("audio-setting-invalid","Invalid volume");AudioState();break;
   case SipCommandKind::MicGain:if(!media.audio.TrySetMicrophoneGain(c.integerArgument))Error("audio-setting-invalid","Invalid gain");AudioState();break;
   case SipCommandKind::EchoCancellation:if(!media.SetEchoCancellation(c.enabled))Error("echo-unavailable","Echo cancellation unavailable");EchoState();break;
   case SipCommandKind::NoiseIsolation:if(!media.audio.TrySetNoiseIsolation(c.enabled))Error("noise-unavailable","Noise processing unavailable");AudioState();break;
   case SipCommandKind::RecordStart:{try{auto result=media.StartRecording({decode(c.recording.captureItemId),c.recording.includeMicrophone,c.recording.includeRoomAudio});if(result.error!=telestudio::RecordingError::None)RecordingError(result.error==telestudio::RecordingError::InvalidTarget?"recording-source-not-shared":"recording-start-rejected",result.error==telestudio::RecordingError::InvalidTarget?"Recording requires the exact acknowledged shared screen or window":"Recording start rejected: code "+std::to_string(int(result.error)),true);else{recordingProtocol=actual;recordingOrigin=true;RecordingState();}}catch(...){RecordingError("recording-start-rejected","Invalid recording target",true);}break;}
   case SipCommandKind::RecordStop:media.StopRecording();break;
   case SipCommandKind::RecordPause:if(media.recorder->Pause().error!=telestudio::RecordingError::None)RecordingError("recording-pause-rejected","Recording not ready");break;
   case SipCommandKind::RecordResume:if(media.ResumeRecording().error!=telestudio::RecordingError::None)RecordingError("recording-resume-rejected","Resume requires an acknowledged shared screen or window and a paused recording");break;
   case SipCommandKind::Camera:media.Camera(c.enabled,c.deviceId);break;
   case SipCommandKind::SharingMode:if(c.transmissionMode!=sharingMode){if(contentWanted)Desktop(false,{},lock);sharingMode=c.transmissionMode;}Text("sharing-mode "+c.argument);break;
   case SipCommandKind::Desktop:Desktop(c.enabled,c.deviceId,lock);break;
   case SipCommandKind::Source:media.Camera(c.argument=="camera");Desktop(c.argument!="camera",c.argument=="camera"?"":decode(c.argument),lock);Emit("source-changed","\"source\":"+quote(c.argument=="camera"?"camera":"desktop"));break;
   case SipCommandKind::CameraOverlay:{auto s=media.settings;s.cameraSize=telestudio::CameraOverlaySize(c.cameraOverlay.size);s.cameraPosition=telestudio::CameraOverlayPosition(c.cameraOverlay.position);media.Configure(s,media.fps);break;}
   case SipCommandKind::VideoPreferences:{auto s=media.settings;int heights[]={720,480,720,1080};s.height=heights[int(c.videoPreferences.resolution)];s.width=s.height==480?854:s.height*16/9;static const char*res[]={"auto","sd","hd","fhd"};media.resolution=res[int(c.videoPreferences.resolution)];if(c.videoPreferences.bitrateKbps)media.bitrateKbps=*c.videoPreferences.bitrateKbps;media.Configure(s,media.fps);break;}
   case SipCommandKind::Identity:{auto name=decode(c.argument);SetDefaultDisplayName(name.c_str());sip->SetDefaultDisplayName(name.c_str());h323->SetDefaultDisplayName(name.c_str());break;}
   case SipCommandKind::Endpoint:if(policy.Active())Error("profile-change-during-call","Endpoint profile locked during call");else{endpoint=c.argument;UpdateOrder();}break;
   case SipCommandKind::AudioProfile:{if(policy.Active()){Error("profile-change-during-call","Audio profile locked during call");break;}auto profile=c.audioProfile;auto reason=telestudio::media::ValidateAudioNetworkProfile(profile,actual==Protocol::Sip);if(!reason.empty()){Error("audio-profile-invalid",reason);break;}if((profile.codec=="aac-ld"||profile.codec=="aac-lc")&&!policy.AacEnabled()){Error("aac-disabled","Enable AAC before selecting its profile");break;}if(actual==Protocol::H323)profile.jitterInitialMs=profile.jitterMinimumMs;audioProfile=profile;UpdateOrder();SetAudioJitterDelay(profile.jitterMinimumMs,profile.jitterMaximumMs);auto jitter=GetJitterParameters();jitter.m_currentJitterDelay=profile.jitterInitialMs;SetJitterParameters(jitter);media.sendBufferMs=profile.sendBufferMs;auto event=telestudio::media::AudioProfileApplied({0,actual==Protocol::Sip?telestudio::media::ConsoleProtocol::Sip:telestudio::media::ConsoleProtocol::H323},profile);if(event){auto pos=event->find("\"codec\"");if(pos!=std::string::npos)Emit("audio-profile-applied",event->substr(pos,event->size()-pos-1));}break;}
   case SipCommandKind::NetworkMode:SetAudioJitterDelay(c.networkMode==SipNetworkMode::External?100:40,c.networkMode==SipNetworkMode::External?300:150);break;
   case SipCommandKind::NetworkVideo:SetVideoContentMode(c.networkVideo.text);media.bitrateKbps=c.networkVideo.bitrateKbps;media.Configure(media.settings,c.networkVideo.fps);{auto event=telestudio::media::NetworkVideoApplied({0,telestudio::media::ConsoleProtocol::Sip},c.networkVideo);if(event){auto pos=event->find("\"fps\"");if(pos!=std::string::npos)Emit("network-video-applied",event->substr(pos,event->size()-pos-1));}}break;
   case SipCommandKind::Codec:if(c.argument!="auto"&&c.argument!="h264")Error("codec-unavailable","OPAL build uses H.264 video");break;
   case SipCommandKind::Quality:media.Configure(media.settings,c.argument=="presentation"?15:30);break;
   default:if(line.starts_with("call ")&&!policy.Active())Failure("invalid-target","Unsupported call target",line.substr(5));else Error("command-invalid","Unsupported or invalid command");break;
  }
 }
};
void StudioManager::AacMarker(RTP_DataFrame&frame,P_INT_PTR){frame.SetMarker(true);}
void StudioManager::IncomingAudio(RTP_DataFrame&frame,P_INT_PTR context){
 auto&stream=reinterpret_cast<OpalMediaPatch*>(context)->GetSource();auto format=stream.GetMediaFormat();
 static thread_local unsigned observed=0;if(observed++<8)PTRACE(3,"OPAL incoming frame size="<<frame.GetPayloadSize()<<" timestamp="<<frame.GetTimestamp()<<" payload="<<frame.GetPayloadType()<<" generation="<<media.generation.load()<<" frame="<<format.GetFrameTime());
 bool knownCodecConflict=false;
 if(frame.GetPayloadSize()&&frame.GetPayloadType()!=format.GetPayloadType()&&frame.GetPayloadType()!=RTP_DataFrame::CN){
  knownCodecConflict=HasKnownAudioPayloadConflict(format,frame.GetPayloadType(),stream.GetConnection().GetMediaFormats());
 }
 if(ShouldDropIncomingAudioPayload(frame.GetPayloadSize(),frame.GetPayloadType(),format.GetPayloadType(),knownCodecConflict)){audioPayloadMismatch=int(frame.GetPayloadType());frame.SetPayloadSize(0);}
 unsigned frames=1;if(str(format.GetName())==str(OpalG722.GetName())){unsigned size=format.GetFrameSize();frames=frame.GetPayloadSize()&&size?(frame.GetPayloadSize()+size-1)/size:format.GetOptionInteger(OpalAudioFormat::TxFramesPerPacketOption(),1);}
 // Pace network reads, including empty jitter output, before transcoding.
 // A codec may discard an empty/CN frame without invoking the PCM callback.
 networkReceivePacing.Wait(media.generation,size_t(format.GetFrameTime())*frames*sizeof(short),format.GetClockRate(),1);
}
SIPConnection* StudioSip::CreateConnection(const SIPConnection::Init&i){return new StudioSipConnection(i,studio);}
void StudioSessionRefreshWorkItem::Work(){PSafePtr<SIPConnection>connection;if(GetTarget(connection))if(auto*studio=dynamic_cast<StudioSipConnection*>(&*connection))studio->RefreshSession();}
void StudioSipConnection::OnReceivedResponse(SIPTransaction&transaction,SIP_PDU&response){
 PSafeLockReadWrite holdLock(*this);if(!holdLock.IsLocked())return;
 HoldResponseGuard holdGuard(*this,transaction);
 if(transaction.GetMethod()==SIP_PDU::Method_INVITE&&response.GetStatusCode()==SIP_PDU::Failure_SessionIntervalTooSmall&&!IsReleased()){
  const auto&mime=response.GetMIME();auto minimum=mime.GetMinSE();
  if(mime.Has("Min-SE")&&minimum>=PTimeInterval(0,90)&&minimum>minimumSessionPeriod&&minimum>transaction.GetMIME().GetSessionExpires()&&sessionIntervalRetries<4){
   SIPTransactionOwner::OnReceivedResponse(transaction,response);
   PSafeLockReadWrite lock(*this);
   if(lock.IsLocked()){
    minimumSessionPeriod=minimum;++sessionIntervalRetries;
    PSafePtr<SIPTransaction>retry=transaction.CreateDuplicate();
    if(retry!=NULL&&retry->Start()){
     if(GetPhase()<ConnectedPhase)m_forkedInvitations.Append(retry);
     else{m_pendingInvitations.Remove(&transaction);m_pendingInvitations.Append(retry);}
     m_handlingINVITE=true;return;
    }
   }
  }
 }
 SIPConnection::OnReceivedResponse(transaction,response);
}
void StudioSipConnection::OnReceivedOK(SIPTransaction&transaction,SIP_PDU&response){
 if(transaction.GetMethod()!=SIP_PDU::Method_INVITE){SIPConnection::OnReceivedOK(transaction,response);return;}
 if(IsReleased())return;
 sessionIntervalRetries=0;
 const auto&mime=response.GetMIME();
 // RFC4028 7.2: Session-Expires negotiates the timer even without Require.
 m_sessionTimerPeriod=mime.GetSessionExpires();
 localSessionRefresher=m_sessionTimerPeriod>0&&SipSessionRefresher(mime)=="uac";
 // Keep OPAL's full interval timer as the expiry watchdog. Only a successful
 // refresh resets it; a rejected offer must not extend the old expiration.
 m_sessionTimerRefresher=false;
 SIPConnection::OnReceivedOK(transaction,response);
 sessionRefreshTimer.Stop(false);
 if(!IsReleased()&&localSessionRefresher)sessionRefreshTimer=PTimeInterval(m_sessionTimerPeriod.GetMilliSeconds()/2);
 studio.Emit("sip-session-timer","\"sessionIntervalSeconds\":"+std::to_string(m_sessionTimerPeriod.GetSeconds())+",\"refreshAfterSeconds\":"+std::to_string(localSessionRefresher?m_sessionTimerPeriod.GetSeconds()/2:0)+",\"localRefresher\":"+(localSessionRefresher?"true":"false"));
}
void StudioSipConnection::AdjustInviteResponse(SIP_PDU&response){
 const bool success=response.GetStatusCode()>=200&&response.GetStatusCode()<300;
 if(success&&m_lastReceivedINVITE){
  const auto&request=m_lastReceivedINVITE->GetMIME();
  auto requestedPeriod=request.GetSessionExpires();
  const bool supported=request.GetSupported().Contains("timer")||request.GetRequire().Contains("timer");
  if(requestedPeriod>0){
   m_sessionTimerPeriod=requestedPeriod;
   // UAS may select either participant when no refresher was supplied. Keep
   // callers responsible by default; explicitly requested uas means us.
   localSessionRefresher=!supported||SipSessionRefresher(request)=="uas";
  }
  if(m_sessionTimerPeriod>0){
   response.GetMIME().SetSessionExpires(m_sessionTimerPeriod,localSessionRefresher?"uas":"uac");
   if(!localSessionRefresher)response.GetMIME().AddRequire("timer");
   m_sessionTimerRefresher=false;
  }
 }
 // OPAL queues a copy for retransmission here, so set final timer headers first.
 SIPConnection::AdjustInviteResponse(response);
 if(success){
  m_sessionTimer.Stop(false);sessionRefreshTimer.Stop(false);
  if(m_sessionTimerPeriod>0){m_sessionTimer=m_sessionTimerPeriod;if(localSessionRefresher)sessionRefreshTimer=PTimeInterval(m_sessionTimerPeriod.GetMilliSeconds()/2);}
  studio.Emit("sip-session-timer","\"sessionIntervalSeconds\":"+std::to_string(m_sessionTimerPeriod.GetSeconds())+",\"refreshAfterSeconds\":"+std::to_string(localSessionRefresher?m_sessionTimerPeriod.GetSeconds()/2:0)+",\"localRefresher\":"+(localSessionRefresher?"true":"false"));
 }
}
bool StudioSipConnection::OnSendOfferSDP(SDPSessionDescription&sdp,bool openOnly){
 bool result=SessionPreservingSipConnection::OnSendOfferSDP(sdp,openOnly);
 if(result){
 // Stock OnSendOfferSDPStream sets the SDP role before FromSession overwrites it
 // with the session's default eNoRole. Preserve the actual negotiated session role.
 for(PINDEX index=1;index<=sdp.GetMediaDescriptions().GetSize();++index){auto*description=sdp.GetMediaDescriptionByIndex(index);if(!description)continue;auto*session=GetMediaSession(description->GetLabel().AsUnsigned());if(!session)continue;auto role=session->GetMediaType()==OpalPresentationVideoMediaDefinition::Name()?OpalVideoFormat::ePresentation:session->GetMediaType()==OpalMediaType::Video()?OpalVideoFormat::eMainRole:OpalVideoFormat::eNoRole;if(role!=OpalVideoFormat::eNoRole){session->SetVideoContentRole(role);description->SetContentRole(role);}}
 if(studio.media.testMode||env("ESSASTUDIO_DIAGNOSTIC_LOG")=="1"){auto text=str(sdp.Encode());studio.Emit("opal-sdp-offer","\"bfcp\":"+std::string(text.find("BFCP")!=std::string::npos?"true":"false")+",\"presentation\":"+(text.find("a=content:slides")!=std::string::npos?"true":"false")+",\"sdp\":"+quote(text),Protocol::Sip);}}return result;}
#if OPAL_BFCP
SDPMediaDescription*StudioSipConnection::OnSendAnswerSDPStream(SDPMediaDescription*offer,unsigned index,bool transfer,BundleMergeInfo&bundle){
 if(!offer||offer->GetMediaType()!=BFCPMediaDefinition::Name()){ScopedVideoSessionRole role(m_activeFormatList,offer?offer->GetContentRole():OpalVideoFormat::eNoRole,offer);return SIPConnection::OnSendAnswerSDPStream(offer,index,transfer,bundle);}
 if(!offer->GetMediaFormats().HasFormat(BFCPMediaFormat::Get())||offer->GetPort()==0)return nullptr;
 OpalTransportAddress localAddress;auto*session=SetUpMediaSession(AnswerMediaSessionId(index),BFCPMediaDefinition::Name(),*offer,localAddress,bundle);if(!session)return nullptr;
 auto*answer=session->CreateSDPMediaDescription();if(!answer)return nullptr;answer->SetIndex(index);answer->SetStringOptions(m_stringOptions);answer->SetSDPTransportType(offer->GetSDPTransportType());answer->AddMediaFormat(BFCPMediaFormat::Get());answer->SetDirection(SDPMediaDescription::SendRecv);bfcpNegotiated=true;return answer;
}
bool StudioSipConnection::OnReceivedAnswerSDPStream(const SDPMediaDescription*answer,unsigned sessionId,bool&multiple,BundleMergeInfo&bundle){
 if(!answer||answer->GetMediaType()!=BFCPMediaDefinition::Name()){ScopedVideoSessionRole role(m_activeFormatList,answer?answer->GetContentRole():OpalVideoFormat::eNoRole,answer);return SIPConnection::OnReceivedAnswerSDPStream(answer,sessionId,multiple,bundle);}
 if(!answer->GetMediaFormats().HasFormat(BFCPMediaFormat::Get())||answer->GetPort()==0)return false;
 OpalTransportAddress localAddress;bfcpNegotiated=SetUpMediaSession(sessionId,BFCPMediaDefinition::Name(),*answer,localAddress,bundle)!=nullptr;return bfcpNegotiated;
}
bool StudioSipConnection::OnBfcpConnected(){SIPConnection::OnBfcpConnected();std::scoped_lock lock(studio.stateMutex);if(studio.contentWanted&&studio.WantsPresentation()&&GetCall().GetToken()==studio.token)studio.pendingBfcpRequest=StudioManager::PendingPresentation{studio.token,studio.generation,studio.contentRevision,studio.desktopId};return true;}
bool StudioSipConnection::OnBfcpDisconnected(){studio.BfcpGranted(*this,false);return SIPConnection::OnBfcpDisconnected();}
bool StudioSipConnection::OnFloorRequestStatusGranted(const BFCPEvent&e){SIPConnection::OnFloorRequestStatusGranted(e);studio.BfcpGranted(*this,true);return true;}
bool StudioSipConnection::OnFloorRequestStatusAborted(const BFCPEvent&e){SIPConnection::OnFloorRequestStatusAborted(e);studio.BfcpGranted(*this,false);return true;}
bool StudioSipConnection::OnFloorStatusGranted(const BFCPEvent&e){SIPConnection::OnFloorStatusGranted(e);const auto user=LocalBfcpParticipantUserId();if(user!=0)studio.BfcpGranted(*this,user==e.m_beneficiaryID);return true;}
bool StudioSipConnection::OnFloorStatusAborted(const BFCPEvent&e){SIPConnection::OnFloorStatusAborted(e);studio.BfcpGranted(*this,false);return true;}
#endif
StudioLocal::StudioLocal(StudioManager&s):OpalLocalEndPoint(s),studio(s){SetDeferredAnswer(true);SetDefaultAudioSynchronicity(e_Synchronous);SetDefaultVideoSourceSynchronicity(e_Synchronous);}
bool StudioLocal::OnIncomingCall(OpalLocalConnection&c){std::scoped_lock lock(studio.stateMutex);auto peer=c.GetOtherPartyConnection();auto url=peer?str(peer->GetRemotePartyURL()):str(c.GetRemotePartyURL());auto protocol=peer&&dynamic_cast<H323Connection*>(&*peer)?Protocol::H323:Protocol::Sip;if(studio.autoAnswer&&!loopback(url))return false;if(!studio.policy.Begin(protocol))return false;studio.actual=protocol;studio.remote=url;studio.token=c.GetCall().GetToken();studio.connected=false;studio.sendAudio=studio.receiveAudio=false;studio.sendCodec.clear();studio.receiveCodec.clear();studio.mediaProtocol=protocol;if(protocol==Protocol::Sip)studio.Text("incoming call from "+url+"; enter answer or reject");else studio.Text("incoming-call remote="+quote(url)+" action=answer-or-reject");try{studio.UpdateOrder();studio.media.Start(++studio.generation);}catch(const std::exception&e){studio.Failure("incoming-media-failed",e.what(),url);return false;}AlertingIncomingCall(studio.token);if(studio.autoAnswer)AcceptIncomingCall(studio.token);return true;}
bool StudioLocal::OnReadMediaData(OpalLocalConnection&,OpalMediaStream&s,void*data,PINDEX size,PINDEX&length){size_t out=0;auto f=s.GetMediaFormat();unsigned channels=f.GetOptionInteger(OpalAudioFormat::ChannelsOption(),1);bool result=studio.media.ReadAudio(data,size,out,f.GetClockRate(),channels);length=PINDEX(out);if(result)sendPacing.Wait(studio.media.generation,out,f.GetClockRate(),channels);return result;}
bool StudioLocal::OnWriteMediaData(const OpalLocalConnection&,const OpalMediaStream&s,const void*data,PINDEX length,PINDEX&written){size_t out=0;auto f=s.GetMediaFormat();unsigned channels=f.GetOptionInteger(OpalAudioFormat::ChannelsOption(),1);bool result=studio.media.WriteAudio(data,length,out,f.GetClockRate(),channels);written=PINDEX(out);return result;}
bool StudioLocal::OnReadMediaFrame(const OpalLocalConnection&connection,const OpalMediaStream&s,RTP_DataFrame&f){auto format=s.GetMediaFormat();if(!format.IsMediaType(OpalMediaType::Video()))return OpalLocalEndPoint::OnReadMediaFrame(connection,s,f);unsigned w=format.GetOptionInteger(OpalVideoFormat::FrameWidthOption(),studio.media.settings.width),h=format.GetOptionInteger(OpalVideoFormat::FrameHeightOption(),studio.media.settings.height);if(w<2||h<2||w>4096||h>2160||w%2||h%2)return false;PThread::Sleep(1000/std::max(1,studio.media.fps));auto ts=uint32_t(GetTickCount64()*90);std::vector<uint8_t>bytes;bool content=IsPresentationStream(connection,s);if(!studio.media.ReadVideo(bytes,w,h,ts,content))return false;f.SetPayloadSize(bytes.size());std::memcpy(f.GetPayloadPtr(),bytes.data(),bytes.size());f.SetTimestamp(ts);f.SetMarker(true);return true;}
bool StudioLocal::OnWriteMediaFrame(const OpalLocalConnection&connection,const OpalMediaStream&s,RTP_DataFrame&f){if(!s.GetMediaFormat().IsMediaType(OpalMediaType::Video()))return OpalLocalEndPoint::OnWriteMediaFrame(connection,s,f);return studio.media.WriteVideo(f.GetPayloadPtr(),f.GetPayloadSize(),IsPresentationStream(connection,s));}
StudioSip::StudioSip(StudioManager&s):SIPEndPoint(s),studio(s){}
void StudioSip::OnRegistrationStatus(const RegistrationStatus&s){SIPEndPoint::OnRegistrationStatus(s);auto status=s.m_reason;std::string state=status==SIP_PDU::Successful_OK?(s.m_wasRegistering?"ok":"cleared"):status<SIP_PDU::Successful_OK?"progress":"failed";studio.Emit("sip-registration-state","\"state\":"+quote(state)+",\"detail\":"+quote(state=="failed"?"OPAL SIP registration failed":""),Protocol::Sip);}
}
class StudioProcess : public PProcess {
 PCLASSINFO(StudioProcess,PProcess);
public:StudioProcess():PProcess("MyStudio","MyStudio",2,0,ReleaseCode,0){} void Main()override{}
};
int main(int argc,char**argv){using namespace studio::opal;
 // This worker's stdin carries commands. An interactive SDK assertion must
 // never consume them or leave its owner waiting for a console answer.
 if(!std::getenv("PTLIB_ASSERT_ACTION")&&!std::getenv("PWLIB_ASSERT_ACTION")&&
    !std::getenv("PTLIB_ASSERT_EXCEPTION")&&!std::getenv("PWLIB_ASSERT_EXCEPTION"))
  _putenv_s("PTLIB_ASSERT_ACTION","A");
 _putenv_s("PTLIBPLUGINDIR",(bin()/"plugins").string().c_str());if(!RegisterMyStudioOpalAacHandler()){std::cerr<<"startup error: AAC handler registration failed\n";return 1;}StudioProcess process;try{
 unsigned sipPort=5140,h323Port=1760,rtpPort=43000;std::string interface;bool autoAnswer=false;
 auto parsePort=[](std::string s){size_t n=0;auto v=std::stoul(s,&n);if(n!=s.size()||v<1||v>65535)throw std::runtime_error("Invalid port");return unsigned(v);};
 for(int i=1;i<argc;++i){std::string arg=argv[i];if(arg=="--help"){std::cout<<"--sip-port 5140 --h323-port 1760 --rtp-port 43000 [--interface IP] [--test-loopback-autoanswer]\n";return 0;}if(arg=="--test-loopback-autoanswer"){autoAnswer=true;continue;}if(arg=="--listen")continue;if(i+1>=argc)throw std::runtime_error("Missing startup argument");std::string value=argv[++i];if(arg=="--sip-port")sipPort=parsePort(value);else if(arg=="--h323-port")h323Port=parsePort(value);else if(arg=="--rtp-port")rtpPort=parsePort(value);else if(arg=="--interface")interface=value;else throw std::runtime_error("Unknown startup argument");}
 if(rtpPort%2||rtpPort>65136)throw std::runtime_error("RTP base leaves no valid TCP control port range");if(autoAnswer&&interface!="127.0.0.1"&&interface!="::1")throw std::runtime_error("Test autoanswer requires explicit loopback interface");

 RegisterH264();PPluginManager::GetPluginManager().LoadPlugin((bin()/"plugins"/(std::filesystem::exists(bin()/"plugins"/"G722_ptplugin.dll")?"G722_ptplugin.dll":"g722_audio.dll")).string().c_str());
 PPluginManager::GetPluginManager().LoadPlugin((bin()/"plugins"/"mystudio_opal_aac.dll").string().c_str());
 bool aacCapabilities=RegisterMyStudioOpalAacCapabilities();
 for(auto name:{"MyStudio-AAC-LD-LATM-64","MyStudio-AAC-LD-LATM-128"}){OpalMediaFormat format(name);if(format.IsValid()){format.SetOptionInteger(OpalMediaFormat::ClockRateOption(),90000);format.SetOptionInteger(OpalMediaFormat::FrameTimeOption(),1800);OpalMediaFormat::SetRegisteredMediaFormat(format);}}
 StudioManager manager;InstallCrashTrace();if(env("ESSASTUDIO_DIAGNOSTIC_LOG")=="1")PTrace::Initialise(4,nullptr,PTrace::Timestamp|PTrace::Thread|PTrace::FileAndLine);SetVideoAcceptance([&](uint32_t ts,bool content,bool accepted,int w,int h,int kbps){manager.media.VideoAccepted(ts,content,accepted,w,h,kbps);});manager.autoAnswer=autoAnswer;manager.transport=env("ESSASTUDIO_SIP_TRANSPORT");if(manager.transport.empty())manager.transport="udp";if(manager.transport!="udp"&&manager.transport!="tcp"&&manager.transport!="tls")throw std::runtime_error("Invalid SIP transport");manager.encryption=env("ESSASTUDIO_SIP_SRTP");if(manager.encryption.empty())manager.encryption="disabled";if(manager.encryption!="disabled"&&manager.encryption!="optional"&&manager.encryption!="required")throw std::runtime_error("Invalid SRTP policy");
#if !OPAL_PTLIB_SSL
 if(manager.transport=="tls")throw std::runtime_error("TLS unavailable in this OPAL build");
#else
 manager.SetSSLCertificateAuthority("*");if(manager.transport=="tls"){auto dataRoot=env("ESSASTUDIO_DATA_ROOT");auto certdir=(dataRoot.empty()?bin()/"OPALData":std::filesystem::u8path(dataRoot))/"Tls";std::filesystem::create_directories(certdir);manager.SetSSLCertificate((certdir/"sip-certificate.pem").string().c_str());manager.SetSSLPrivateKey((certdir/"sip-private-key.pem").string().c_str());}
#endif

 manager.SetDefaultUserName("mystudio");manager.SetDefaultDisplayName("MyStudio");manager.SetRtpIpPorts(rtpPort,rtpPort+198);manager.SetTCPPorts(rtpPort+200,rtpPort+398);manager.SetAudioJitterDelay(40,150);manager.SetAutoStartTransmitVideo(true);manager.SetAutoStartReceiveVideo(true);OpalMediaType(OpalPresentationVideoMediaDefinition::Name())->SetAutoStart(OpalMediaType::Receive);
 manager.local=new StudioLocal(manager);manager.sip=new StudioSip(manager);manager.h323=new H323EndPoint(manager);ConfigureH323Content(*manager.h323);PStringArray security;auto suites=manager.sip->GetAllMediaCryptoSuites();if(manager.encryption!="disabled")for(auto&suite:suites)if(suite!=OpalMediaCryptoSuite::ClearText())security.AppendString(suite);if(manager.encryption!="disabled"&&security.IsEmpty())throw std::runtime_error("SRTP policy requested but no crypto suite is compiled");if(manager.encryption!="required")security.AppendString(OpalMediaCryptoSuite::ClearText());manager.sip->SetMediaCryptoSuites(security);if(manager.encryption!="disabled")manager.sip->SetDefaultStringOption(OPAL_OPT_UNSECURE_SRTP,"true");PStringArray clear;clear.AppendString(OpalMediaCryptoSuite::ClearText());manager.h323->SetMediaCryptoSuites(clear);

 manager.AddRouteEntry("sip:.*=local:");manager.AddRouteEntry("h323:.*=local:");
 auto binding=interface.empty()?"*":interface;PIPSocket::Address listenAddress=interface.empty()?PIPSocket::GetDefaultIpAny():PIPSocket::Address(interface.c_str());if(!listenAddress.IsValid())throw std::runtime_error("Invalid listener interface");
 // Address-string listener creation in this SDK flips exclusive into reuse.
 // Construct explicit exclusive listeners so a second worker cannot steal ports.
 bool sipBound=false;
#if OPAL_PTLIB_SSL
 if(manager.transport=="tls")sipBound=manager.sip->StartListener(new OpalListenerTLS(*manager.sip,listenAddress,WORD(sipPort),true));else
#endif
 sipBound=manager.sip->StartListener(new OpalListenerUDP(*manager.sip,listenAddress,WORD(sipPort),true))&&manager.sip->StartListener(new OpalListenerTCP(*manager.sip,listenAddress,WORD(sipPort),true));
 if(!sipBound)throw std::runtime_error("SIP listener could not bind; check port conflict");if(!manager.h323->StartListener(new OpalListenerTCP(*manager.h323,listenAddress,WORD(h323Port),true)))throw std::runtime_error("H.323 listener could not bind; check port conflict");
 manager.aacAvailable=aacCapabilities;for(auto name:{"MyStudio-AAC-LD-LATM-64","MyStudio-AAC-LD-LATM-128","MyStudio-AAC-LC-48k-stereo192","MyStudio-AAC-LC-48k-mono96"}){OpalMediaFormat format(name);format.SetOptionString(OpalMediaFormat::ProtocolOption(),"SIP");OpalMediaFormat pcm=GetOpalPCM16(48000,format.GetOptionInteger(OpalAudioFormat::ChannelsOption(),1));std::unique_ptr<OpalTranscoder>encode(OpalTranscoder::Create(pcm,format)),decode(OpalTranscoder::Create(format,pcm));if(!format.IsValid()||!encode||!decode)manager.aacAvailable=false;}
 if(!manager.aacAvailable)manager.policy.ConfigureAac("off",false);
 manager.Availability(Protocol::Sip);manager.Availability(Protocol::H323);
 auto registrar=env("ESSASTUDIO_SIP_REGISTRAR"),identity=env("ESSASTUDIO_SIP_IDENTITY"),proxy=env("ESSASTUDIO_SIP_OUTBOUND_PROXY"),username=env("ESSASTUDIO_SIP_USERNAME"),password=env("ESSASTUDIO_SIP_PASSWORD");if(!proxy.empty())manager.sip->SetProxy(SIPURL(proxy.c_str()));if(!identity.empty())manager.sip->SetDefaultLocalPartyName(identity.c_str());if(!registrar.empty()){SIPRegister::Params params;params.m_addressOfRecord=identity.c_str();params.m_registrarAddress=registrar.c_str();params.m_authID=username.c_str();params.m_password=password.c_str();PString aor;if(!manager.sip->Register(params,aor))manager.Error("registration-config-invalid","Unable to create OPAL registration");}else manager.Emit("sip-registration-state","\"state\":\"none\",\"detail\":\"\"",Protocol::Sip);if(!password.empty())SecureZeroMemory(password.data(),password.size());SetEnvironmentVariableA("ESSASTUDIO_SIP_PASSWORD",nullptr);
 std::unique_ptr<OpalTranscoder>audioProbe(OpalTranscoder::Create(OpalPCM16_16KHZ,OpalG722));if(!audioProbe)throw std::runtime_error("G.722 codec plugin required");
 manager.Emit("engine-ready","\"engine\":\"opal\",\"version\":\"4.0\",\"codePage\":"+std::to_string(GetACP()));manager.Text("Listening SIP on "+binding+":"+std::to_string(sipPort));manager.Text("Listening H.323 on "+binding+":"+std::to_string(h323Port));manager.EchoState();
 std::mutex mutex;std::condition_variable cv;std::deque<std::string>commands;bool closed=false;std::thread reader([&]{std::string line;while(ReadCommand(std::cin,line)){bool quit=line=="quit"||line=="exit";{std::unique_lock lock(mutex);cv.wait(lock,[&]{return commands.size()<64;});commands.push_back(std::move(line));}if(quit)break;}std::scoped_lock lock(mutex);closed=true;});
 auto next=std::chrono::steady_clock::now()+std::chrono::seconds(1);bool running=true;while(running){std::string line;{std::scoped_lock lock(mutex);if(!commands.empty()){line=std::move(commands.front());commands.pop_front();cv.notify_one();}else if(closed)running=false;}if(line=="quit"||line=="exit")break;if(!line.empty())try{manager.Command(line);}catch(const std::exception&e){manager.Error("command-failed",e.what());}manager.PumpHold();manager.PumpDtmf();manager.PumpBfcpRequest();manager.PumpPresentation();manager.PumpContent();{std::scoped_lock pumpLock(manager.stateMutex);manager.media.Pump([&](auto type,auto fields){bool recording=type.starts_with("recording-");std::scoped_lock lock(manager.stateMutex);manager.Emit(type,fields,recording&&manager.recordingOrigin?manager.recordingProtocol:manager.mediaProtocol);if(type=="recording-state"&&(fields.find("\"state\":\"completed\"")!=std::string::npos||fields.find("\"state\":\"failed\"")!=std::string::npos)){manager.recordingOrigin=false;if(manager.connected&&!manager.recordingControls){manager.MediaReady();manager.RecordingState();}}},manager.connected);}if(std::chrono::steady_clock::now()>=next){manager.Diagnostics();manager.VideoDiagnostics();next=std::chrono::steady_clock::now()+std::chrono::seconds(1);}std::this_thread::sleep_for(std::chrono::milliseconds(10));}
 reader.join();manager.ShutDownEndpoints();manager.media.Stop();SetVideoAcceptance({});return 0;
 }catch(const std::exception&e){std::cerr<<"startup error: "<<e.what()<<std::endl;return 1;}}
