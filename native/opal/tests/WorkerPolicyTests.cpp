#include "../WorkerPolicy.h"
#include <iostream>
#include <stdexcept>
void require(bool value,const char* what) { if(!value) throw std::runtime_error(what); }
int main() { try {
 studio::opal::WorkerPolicy p;
 require(p.AacEnabled(),"AAC defaults enabled when the plugin is available");
 require(p.Select("h323"),"idle protocol changes must work");
 require(p.Selected()==studio::opal::Protocol::H323,"protocol must update");
 require(!p.Select("invalid"),"unknown protocols rejected");
 require(!p.ConfigureAac("on",false),"missing plugin rejects AAC");
 require(p.ConfigureAac("on",true)&&p.AacEnabled(),"ready plugin enables AAC");
 require(p.Begin(studio::opal::Protocol::Sip),"incoming protocol begins while idle");
 require(p.Selected()==studio::opal::Protocol::Sip,"incoming call selects actual protocol");
 require(!p.Begin(studio::opal::Protocol::H323),"only one call allowed");
 require(!p.Select("h323")&&!p.ConfigureAac("off",true),"protocol and AAC locked in call");
 p.End(); require(p.ConfigureAac("off",false)&&!p.AacEnabled(),"AAC can always disable while idle");
 require(!p.ConfigureAac("maybe",true),"invalid AAC value rejected");
 std::cout<<"OPAL policy PASS\n"; return 0;
 } catch(const std::exception&e){ std::cerr<<e.what()<<'\n';return 1;} }
