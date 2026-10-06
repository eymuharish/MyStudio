#pragma once
#include <ep/localep.h>
namespace studio::opal {
class PcmMediaStream : public OpalLocalMediaStream {
 PCLASSINFO(PcmMediaStream,OpalLocalMediaStream);
public:
 using OpalLocalMediaStream::OpalLocalMediaStream;
 PBoolean ReadData(BYTE*data,PINDEX size,PINDEX&length)override{
  const auto timestamp=GetTimestamp();
  if(!OpalLocalMediaStream::ReadData(data,size,length))return false;
  const unsigned channels=GetMediaFormat().GetOptionInteger(OpalAudioFormat::ChannelsOption(),1);
  if(!channels||length%(channels*sizeof(short)))return false;
  // Stock localep.cxx multiplies before its ceil division and adds nearly
  // one extra millisecond per read. PCM timestamps are exact sample frames,
  // including the fractional-millisecond 1024-sample AAC-LC operating point.
  SetTimestamp(timestamp+length/(channels*sizeof(short)));
  return true;
 }
};
class PcmLocalConnection : public OpalLocalConnection {
 PCLASSINFO(PcmLocalConnection,OpalLocalConnection);
public:
 using OpalLocalConnection::OpalLocalConnection;
 OpalMediaStream*CreateMediaStream(const OpalMediaFormat&format,unsigned session,PBoolean source)override{
  if(format.IsMediaType(OpalMediaType::Audio())&&!format.IsTransportable())
   return new PcmMediaStream(*this,format,session,source,GetSynchronicity(format,source));
  return OpalLocalConnection::CreateMediaStream(format,session,source);
 }
};
}
