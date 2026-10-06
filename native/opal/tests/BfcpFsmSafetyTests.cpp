#include "BFCP_fsm.h"
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
class LocalControlFsm : public BFCP_fsm {
public:
 bool sendAccepted=false;std::string lastLog;
 LocalControlFsm(){setName("owned-controls");}
 void State(e_BFCP_ACT value){m_bfcpFsmSt=value;}
 bool BFCPFSM_FloorRequest(s_bfcp_msg_event*)override{return sendAccepted;}
 bool BFCPFSM_FloorRelease(s_bfcp_msg_event*)override{return sendAccepted;}
 void Log(const char*,int,int,const char*format,...)override{
  char text[1024];va_list args;va_start(args,format);vsnprintf(text,sizeof(text),format,args);va_end(args);lastLog=text;
 }
};
extern "C" int mystudio_bfcp_fsm_safety_tests(){
 LocalControlFsm fsm;fsm.State(BFCP_fsm::BFCP_ACT_FloorRequest);
 if(fsm.FsmCtrlPerform(BFCP_fsm::BFCP_ACT_FloorRelease,nullptr))return 21; // Stop before grant: no-op safely rejects.
 fsm.FsmCtrlPerform(BFCP_fsm::BFCP_ACT_FloorRequest,nullptr); // A repeated request may retain pending state.
 if(fsm.GetFSMState()!=BFCP_fsm::BFCP_ACT_FloorRequest)return 22;
 fsm.State(BFCP_fsm::BFCP_ACT_CONNECTED);
 if(fsm.FsmCtrlPerform(BFCP_fsm::BFCP_ACT_FloorRequest,nullptr)||fsm.GetFSMState()!=BFCP_fsm::BFCP_ACT_CONNECTED)return 23;
 s_bfcp_msg_event local={};fsm.sendAccepted=true;
 if(!fsm.FsmCtrlPerform(BFCP_fsm::BFCP_ACT_FloorRequest,&local)||fsm.GetFSMState()!=BFCP_fsm::BFCP_ACT_FloorRequest)return 24;
 if(fsm.lastLog.find("FSM [owned-controls]")==std::string::npos)return 25;
 puts("BFCP pending stop/repeated request safe; failed send retains state; success log bounded PASS");return 0;
}
