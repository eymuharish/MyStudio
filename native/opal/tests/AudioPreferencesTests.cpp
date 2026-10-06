#include <algorithm>
#include "../AudioPreferences.h"
#include <iostream>
#include <stdexcept>
using namespace studio::opal;
void check(bool value){if(!value)throw std::runtime_error("Audio preference regression");}
int main(){telestudio::media::AudioNetworkProfile p;
 check(AudioOrder(true,true,"sx20",p).front()=="MyStudio-AAC-LD-LATM-128");
 auto cisco=AudioOrder(true,true,"sx20",p);check(cisco[1]=="MyStudio-AAC-LD-LATM-64"&&cisco[2]=="G.722-64k");
 check(AllowsAac("MyStudio-AAC-LD-LATM-128",true,true,"sx20",p));
 check(AudioOrder(false,true,"sx20",p).front()=="MyStudio-AAC-LD-LATM-128");
 check(AudioOrder(true,true,"pcs-xg77",p).front()=="MyStudio-AAC-LC-48k-stereo192");
 auto sony=AudioOrder(true,true,"pcs-xg77",p);check(sony[1]=="MyStudio-AAC-LC-48k-mono96"&&sony[2]=="G.722-64k");
 p.codec="aac-ld";p.channels=1;check(AudioOrder(false,true,"pcs-xg77",p).front()=="MyStudio-AAC-LD-LATM-64");
 p.codec="aac-lc";p.channels=1;check(AudioOrder(false,true,"sx20",p).front()=="MyStudio-AAC-LC-48k-mono96");
 check(AudioOrder(true,false,"pcs-xg77",p).front()=="G.722-64k");
 std::cout<<"OPAL audio preferences PASS\n";}
