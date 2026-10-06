#pragma once
#include <functional>
#include <cstdint>
namespace studio::opal {
void RegisterH264();
void SetVideoContentMode(bool text);
using VideoAcceptance=std::function<void(uint32_t,bool,bool,int,int,int)>;
void SetVideoAcceptance(VideoAcceptance);
}
