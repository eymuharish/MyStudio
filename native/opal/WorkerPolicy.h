#pragma once
#include <string>
#include <string_view>
namespace studio::opal {
enum class Protocol { Sip, H323 };
class WorkerPolicy {
public:
 Protocol Selected() const { return selected_; }
 bool AacEnabled() const { return aac_; }
 bool Active() const { return active_; }
 bool Select(std::string_view value) {
  if(active_||(value!="sip"&&value!="h323")) return false;
  selected_=value=="sip"?Protocol::Sip:Protocol::H323; return true;
 }
 bool ConfigureAac(std::string_view value, bool available) {
  if(active_||(value!="on"&&value!="off")||(value=="on"&&!available))return false;
  aac_=value=="on";return true;
 }
 bool Begin(Protocol actual) {
  if(active_)return false; selected_=actual; active_=true;return true;
 }
 void End() { active_=false; }
private:
 Protocol selected_=Protocol::Sip;
 bool aac_=true, active_=false;
};
}
