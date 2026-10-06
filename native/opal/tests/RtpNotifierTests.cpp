#include <ptlib.h>
#include <rtp/rtp_session.h>
#include <iostream>
class Registry : public OpalRTPSession {public:using OpalRTPSession::NotifierMap;};
class Target : public PObject {
 PDECLARE_RTPDataNotifier(Target,Received);
public:OpalRTPSession::DataNotifier Callback(){return PCREATE_RTPDataNotifier(Received);}
};
void Target::Received(OpalRTPSession&,OpalRTPSession::Data&){}
int main(){Target first,second;Registry::NotifierMap entries;auto a=first.Callback(),b=second.Callback();entries.Add(100,a);entries.Add(50,b);entries.Add(100,a);if(entries.size()!=2){std::cerr<<"Copied RTP callback identity was duplicated\n";return 1;}entries.Remove(a);if(entries.size()!=1||entries.begin()->second.GetTarget()!=&second){std::cerr<<"Closed stream RTP callback remained registered\n";return 1;}entries.Remove(b);if(!entries.empty())return 1;std::cout<<"RTP notifier lifetime PASS duplicate/release isolated by actual target\n";}
