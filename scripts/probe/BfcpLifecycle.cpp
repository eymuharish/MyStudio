#include <ptlib.h>
#include <ptlib/pprocess.h>
#include <bfcpsrvctl/bfcpsrv/bfcp_server.h>
#include <bfcpsrvctl/bfcpcli/bfcp_participant.h>
#include <iostream>
extern "C" void _ServerLog(char*,int,int,char*,...);
extern "C" void _ParticpantLog(char*,int,int,char*,...);
class Probe : public PProcess {
public:
 bool failedFloorRequest = false;
 bool failedTcpConnect = false;
 Probe():PProcess("MyStudio","BFCP lifecycle probe",1,0,ReleaseCode,0){}
 void Main() override {
  for(unsigned n=0;n<(failedTcpConnect?4:32);++n) {
   if(failedTcpConnect) {
    auto* participant=new BFCP_Participant(1,2,2,3,nullptr,BFCP_OVER_TCP);
    auto result=participant->OpenTcpConnection("127.0.0.1",0,"127.0.0.1",1,BFCPConnectionRole::ACTIVE);
    if(result)throw std::runtime_error("Closed fixture port unexpectedly connected");
    delete participant;std::cerr<<"BFCP_FAILED_TCP_CLEAN "<<n<<'\n';continue;
   }
   if(failedFloorRequest) {
    auto* participant=new BFCP_Participant(1,2,2,3,nullptr,BFCP_OVER_UDP);
    auto result=participant->bfcp_floorRequest_participant(2);
    if(result!=-1)throw std::runtime_error("Unconnected floor request unexpectedly succeeded");
    delete participant;std::cerr<<"BFCP_FAILED_REQUEST_CLEAN "<<n<<'\n';continue;
   }
   auto* server=new BFCP_Server(1,1,2,1,3,nullptr,BFCP_OVER_UDP);
   std::cerr<<"BFCP_CREATED "<<n<<'\n';delete server;
   std::cerr<<"BFCP_DELETED "<<n<<'\n';
  }
 }
};
int main(int argc,char** argv) {
 SetEnvironmentVariableA("PTLIBPLUGINDIR","plugins");Probe process;
 process.failedFloorRequest=argc>1&&std::string(argv[1])=="failed-floor";
 process.failedTcpConnect=argc>1&&std::string(argv[1])=="failed-tcp";
 if(argc>1) {
  char file[]="lifecycle-probe",format[]="%s";
  if(std::string(argv[1])=="participant")_ParticpantLog(file,1,0,format,"BFCP participant logger lifecycle");
  else _ServerLog(file,1,0,format,"BFCP server logger lifecycle");
 }
 process.Main();return 0;
}
