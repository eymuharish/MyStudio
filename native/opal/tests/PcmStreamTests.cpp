#include <ptlib.h>
#include <ptlib/pprocess.h>
#include <opal/manager.h>
#include <opal/call.h>
#include <ep/localep.h>
#include "../PcmMediaStream.h"
#include <iostream>
#include <vector>
class Process : public PProcess {PCLASSINFO(Process,PProcess);public:Process():PProcess("MyStudio","PcmClock",1,0,ReleaseCode,0){}void Main()override{}};
class Fixture : public OpalLocalEndPoint {
public:Fixture(OpalManager&m):OpalLocalEndPoint(m){}
 void Adopt(OpalConnection*c){AddConnection(c);}
 bool OnReadMediaData(OpalLocalConnection&,OpalMediaStream&,void*data,PINDEX size,PINDEX&length)override{std::memset(data,0,size);length=size;return true;}
};
int main(){_putenv_s("PTLIBPLUGINDIR","MyStudio-no-auto-plugins");Process process;OpalManager manager;auto*endpoint=new Fixture(manager);auto*call=manager.InternalCreateCall();auto*connection=new OpalLocalConnection(*call,*endpoint,nullptr,0,nullptr);endpoint->Adopt(connection);bool passed=true;
 for(auto item:{std::tuple{16000u,1u,320u},std::tuple{48000u,1u,960u},std::tuple{48000u,2u,960u},std::tuple{48000u,2u,1024u},std::tuple{32000u,1u,1024u}}){auto[rate,channels,samples]=item;auto format=GetOpalPCM16(rate,channels);studio::opal::PcmMediaStream stream(*connection,format,1,true,OpalLocalEndPoint::e_Synchronous);stream.SetTimestamp(0);std::vector<BYTE>buffer(samples*channels*sizeof(short));PINDEX read=0;for(unsigned n=1;n<=64;++n){if(!stream.ReadData(buffer.data(),buffer.size(),read)||read!=buffer.size()||stream.GetTimestamp()!=n*samples){std::cerr<<"PCM timestamp drift rate="<<rate<<" channels="<<channels<<" samples="<<samples<<" frame="<<n<<" actual="<<stream.GetTimestamp()<<" expected="<<n*samples<<'\n';passed=false;break;}}}
 manager.ClearAllCalls(OpalConnection::EndedByLocalUser,true);manager.ShutDownEndpoints();if(passed)std::cout<<"Actual PCM stream timestamps PASS G722/LD/LC mono/stereo\n";return passed?0:1;
}
