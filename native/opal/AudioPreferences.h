#pragma once
#include <telestudio/media/NetworkProfiles.h>
#include <string>
#include <algorithm>
#include <vector>
namespace studio::opal {
inline std::vector<std::string> AudioOrder(bool sip,bool enabled,const std::string&endpoint,const telestudio::media::AudioNetworkProfile&p){
 std::vector<std::string> order;
 if(enabled&&p.codec!="g722"){
  bool lc=p.codec=="aac-lc"||(p.codec=="auto"&&(endpoint=="pcs-xg77"||endpoint=="pcs-xg80"));
  if(lc){
   bool mono=p.codec=="aac-lc"&&p.channels==1;
   if(mono)order.emplace_back("MyStudio-AAC-LC-48k-mono96");
   order.emplace_back("MyStudio-AAC-LC-48k-stereo192");
   if(!mono)order.emplace_back("MyStudio-AAC-LC-48k-mono96");
   if(!sip){order.emplace_back("MyStudio-AAC-LC-32k-stereo192");order.emplace_back("MyStudio-AAC-LC-32k-mono96");}
  }else{
   bool mono=p.codec=="aac-ld"&&p.channels==1;
   if(mono)order.emplace_back("MyStudio-AAC-LD-LATM-64");
   order.emplace_back("MyStudio-AAC-LD-LATM-128");
   if(!mono)order.emplace_back("MyStudio-AAC-LD-LATM-64");
  }
 }
 order.emplace_back("G.722-64k");bool sony=sip&&(endpoint=="pcs-xg77"||endpoint=="pcs-xg80");if(sony)order.emplace_back("H.264-0");order.emplace_back("H.264-1");if(sip&&!sony)order.emplace_back("H.264-0");return order;
}
inline bool AllowsAac(const std::string&name,bool sip,bool enabled,const std::string&endpoint,const telestudio::media::AudioNetworkProfile&p){
 auto order=AudioOrder(sip,enabled,endpoint,p);return std::find(order.begin(),order.end(),name)!=order.end();
}
}
