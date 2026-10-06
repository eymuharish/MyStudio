#include <ptlib.h>
#include <ptlib/pprocess.h>
#include <opal/manager.h>
#include <opal/call.h>
#include <opal/connection.h>
#include <sip/sipep.h>
#include <atomic>
#include <iostream>
#include <memory>
#include <fstream>
#include <sstream>
#include <stdexcept>

class Process : public PProcess {
 PCLASSINFO(Process,PProcess);
public: Process():PProcess("MyStudio","CallEnumeration",1,0,ReleaseCode,0){}void Main()override{}
};
class EnumeratedCall : public OpalCall {
public:
 explicit EnumeratedCall(OpalManager& manager):OpalCall(manager){}
 bool Next(PSafeArray<OpalConnection>::iterator& iterator,PSafePtr<OpalConnection>& result,PSafetyMode mode,const OpalConnection* skip){return EnumerateConnections(iterator,result,mode,skip);}
};
class OwnedConnection : public OpalConnection {
public:
 OwnedConnection(OpalCall& call,OpalEndPoint& endpoint,const char* token):OpalConnection(call,endpoint,token){}
 ~OwnedConnection()override{SafeRemove();}
 bool IsNetworkConnection()const override{return true;}
};
class RemovalTraceBuffer : public std::stringbuf {
 std::ofstream output{"test-evidence/call-enumeration-sdk.log"};
 OpalConnection* target=nullptr;std::string address;
public:
 bool injected=false;
 void Arm(OpalConnection& connection){target=&connection;std::ostringstream text;text<<static_cast<void*>(target);address=text.str();injected=false;}
 int sync()override{
  const auto line=str();str("");output<<line<<std::flush;
  // SafeReference logs after releasing its safety mutex. Remove immediately
  // after the snapshot acquired a valid reference, before begin() obtains its
  // initial iterator reference. No production timing hook is required.
  if(target&&line.find(address)!=std::string::npos&&line.find("incremented reference count")!=std::string::npos){auto* retiring=target;target=nullptr;injected=true;retiring->SafeRemove();}
  return 0;
 }
};
class RemovalTraceStream : public std::ostream {
public:
 RemovalTraceBuffer* buffer;
 RemovalTraceStream():std::ostream(new RemovalTraceBuffer),buffer(static_cast<RemovalTraceBuffer*>(rdbuf())){}
 ~RemovalTraceStream(){rdbuf(nullptr);delete buffer;}
};
class InvalidatingPointer : public PSafePtr<OpalConnection> {
 OpalConnection& retiring;
 void Invalidate(PSafeObject* object){if(object==&retiring&&!injected){injected=true;retiring.SafeRemove();}}
public:
 bool injected=false;
 explicit InvalidatingPointer(OpalConnection& target):retiring(target){}
 void Assign(PSafeObject* object)override{Invalidate(object);PSafePtrBase::Assign(object);}
 void Assign(const PSafePtrBase& pointer)override{Invalidate(pointer.GetObjectAs<OpalConnection>());PSafePtrBase::Assign(pointer);}
};
static void CheckInvalidation(OpalManager& manager,OpalEndPoint& endpoint,RemovalTraceBuffer& trace,PSafetyMode mode,bool initialIterator){
 auto* call=new EnumeratedCall(manager);PSafePtr<OpalCall> hold(call,PSafeReference);
 {
  auto retiring=std::make_unique<OwnedConnection>(*call,endpoint,"retiring"),keeper=std::make_unique<OwnedConnection>(*call,endpoint,"keeper");
  if(call->GetConnectionCount()!=2||call->GetOtherPartyConnection(*keeper).GetObject()!=retiring.get())throw std::runtime_error("Valid peer enumeration failed before invalidation");
  PSafeArray<OpalConnection>::iterator iterator;
  InvalidatingPointer result(*retiring);
  if(initialIterator)trace.Arm(*retiring);
  const bool found=call->Next(iterator,result,mode,keeper.get());
  if(found||result.GetObject()!=nullptr)throw std::runtime_error("A removed peer was returned by actual SDK enumeration");
  if(initialIterator?!trace.injected:!result.injected)throw std::runtime_error("The intended reference invalidation boundary was not exercised");
  if(call->GetOtherPartyConnection(*keeper)!=nullptr)throw std::runtime_error("Removed peer lookup must remain empty");
  keeper->SafeRemove(); // Explicit owners delete both objects after all safe references leave scope.
 }
 call->InternalOnClear();hold.SetNULL();
 std::cout<<(initialIterator?"Initial iterator":"Result assignment")<<" reference invalidation mode="<<mode<<" safely skipped PASS\n";
}
int main(){
 _putenv_s("PTLIBPLUGINDIR","MyStudio-no-auto-plugins");_putenv_s("PTLIB_ASSERT_ACTION","A");Process process;
 try{
  // Construct before enabling verbose trace: stock manager trace probes video
  // device capabilities during its constructor. This fixture uses no devices.
  OpalManager manager;auto* endpoint=new SIPEndPoint(manager);
  PTrace::Initialise(7,nullptr,PTrace::Timestamp|PTrace::Thread|PTrace::FileAndLine);auto* trace=new RemovalTraceStream;PTrace::SetStream(trace);
  for(auto mode:{PSafeReference,PSafeReadOnly,PSafeReadWrite}){CheckInvalidation(manager,*endpoint,*trace->buffer,mode,true);CheckInvalidation(manager,*endpoint,*trace->buffer,mode,false);}
  manager.ShutDownEndpoints();PTrace::SetStream(nullptr);return 0;
 }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
