#pragma once
#include <atomic>
#include <sdp/sdp.h>

namespace studio::opal {
// RFC 3264: a failed offer/answer leaves the previously negotiated session intact.
// Stream callbacks populate a candidate; a bodyless response cannot replace it.
class BfcpNegotiationTransaction {
 std::atomic<bool>& negotiated;
 const bool previous;
 const bool hasSdp;
 bool committed=false;
public:
 BfcpNegotiationTransaction(std::atomic<bool>& state,const SDPSessionDescription* sdp)
  :negotiated(state),previous(state.load()),hasSdp(sdp!=nullptr){if(hasSdp)negotiated=false;}
 void Complete(bool accepted){committed=hasSdp&&accepted;}
 ~BfcpNegotiationTransaction(){if(!committed)negotiated=previous;}
 BfcpNegotiationTransaction(const BfcpNegotiationTransaction&)=delete;
};
}
