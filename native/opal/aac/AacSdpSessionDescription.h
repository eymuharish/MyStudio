#pragma once
#include <sdp/sdp.h>
#include <map>

namespace studio::opal {
inline bool IsExactSupportedLdStereo(const PString& fmtp){
 std::map<PCaselessString,PString> values;
 for(const auto& field:fmtp.Tokenise(';',false)){
  const auto equal=field.Find('=');if(equal==P_MAX_INDEX)return false;
  const PCaselessString key=field.Left(equal).Trim();
  if(!values.emplace(key,field.Mid(equal+1).Trim()).second)return false;
 }
 // LATM config is a StreamMuxConfig, not a plain ASC. This observed-peer
 // exception deliberately refuses any unvalidated out-of-band configuration.
 return values.count("config")==0&&values.count("MPS-asc")==0&&values["profile-level-id"]=="25"&&values["object"]=="23"&&values["bitrate"]=="128000";
}

class AacSdpSessionDescription : public SDPSessionDescription {
public:
 using SDPSessionDescription::SDPSessionDescription;
 using SDPSessionDescription::Decode;
 bool Decode(const PStringArray& lines,const OpalMediaFormatList& formats)override{
  const auto local=formats.FindFormat("MyStudio-AAC-LD-LATM-128");
  if(local==formats.end()||local->GetOptionInteger(OpalAudioFormat::ChannelsOption(),1)!=2)
   return SDPSessionDescription::Decode(lines,formats);
  PStringArray normalized=lines;normalized.MakeUnique();
  for(PINDEX begin=0;begin<lines.GetSize();){
   PINDEX end=begin+1;while(end<lines.GetSize()&&lines[end].Left(2)!="m=")++end;
   if(lines[begin].Left(8)=="m=audio ")for(PINDEX index=begin+1;index<end;++index){
    if(lines[index].Left(9)!="a=rtpmap:")continue;
    const auto fields=lines[index].Mid(9).Trim().Tokenise(" \t",false);
    if(fields.GetSize()!=2||PCaselessString(fields[1])!="MP4A-LATM/90000")continue;
    const PString rtpPrefix="a=rtpmap:"+fields[0]+" ",fmtpPrefix="a=fmtp:"+fields[0]+" ";
    unsigned rtpCount=0,fmtpCount=0;bool exact=false;
    for(PINDEX scan=begin+1;scan<end;++scan){
     if(lines[scan].Left(9)=="a=rtpmap:"){
      const auto candidate=lines[scan].Mid(9).Trim().Tokenise(" \t",false);
      if(candidate.GetSize()>0&&candidate[0]==fields[0])++rtpCount;
     }
     if(lines[scan].Left(fmtpPrefix.GetLength())==fmtpPrefix){++fmtpCount;exact=IsExactSupportedLdStereo(lines[scan].Mid(fmtpPrefix.GetLength()));}
    }
    // SX20 omits /channels while explicitly selecting our exact LD stereo
    // operating point. Explicit /1, other profiles and other sections retain
    // the stock SDP interpretation. Do not infer stereo from PT or bitrate alone.
    if(rtpCount==1&&fmtpCount==1&&exact)normalized[index]=rtpPrefix+fields[1]+"/2";
   }
   begin=end;
  }
  if(!SDPSessionDescription::Decode(normalized,formats))return false;
  for(PINDEX index=1;index<=GetMediaDescriptions().GetSize();++index){
   auto* description=GetMediaDescriptionByIndex(index);
   if(!description||description->GetMediaType()!=OpalMediaType::Audio())continue;
   for(const auto& item:description->GetSDPMediaFormats()){
    const auto& format=item.GetMediaFormat();
    // A fixed stereo operating point cannot encode an explicit mono rtpmap.
    // Leave other offered candidates available for normal mono/G.722 fallback.
    if(format.GetName()=="MyStudio-AAC-LD-LATM-128"&&format.GetOptionInteger(OpalAudioFormat::ChannelsOption(),1)!=2)
     const_cast<SDPMediaFormat&>(item).GetWritableMediaFormat()=OpalMediaFormat();
   }
  }
  return true;
 }
};
}
