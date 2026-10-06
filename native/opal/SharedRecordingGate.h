#pragma once
#include <cstdint>
#include <string>
#include <string_view>
namespace studio::opal {
// Owner serializes this gate with the outgoing video queue and acknowledgments.
class SharedRecordingGate {
 std::string source;uint64_t revision=0;bool confirmed=false;
public:
 uint64_t Select(std::string value){source=std::move(value);confirmed=false;return ++revision;}
 uint64_t Revision()const{return revision;}
 const std::string&Source()const{return source;}
 bool Confirm(uint64_t value,bool accepted){if(value!=revision||source.empty())return false;confirmed=accepted;return confirmed;}
 bool CanRecord(std::string_view value)const{return confirmed&&!source.empty()&&value==source;}
};
}
