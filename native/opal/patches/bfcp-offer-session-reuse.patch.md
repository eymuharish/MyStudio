BFCP established session reuse during offer construction

Apply after the existing BFCP fixes. Stock SDP offer construction calls Open with GetRemoteMediaAddress, which in SIP contains the signalling host with no negotiated BFCP port. After a genuine floor2 grant, opening the dedicated presentation stream triggers re-INVITE and replaces the working BFCP peer address with127.0.0.1:1, disconnecting the granted session. Source: sdpep.cxx702 and SIPConnection::GetRemoteMediaAddress. Actual owned evidence: loopback-eacf-bfcp-granted/sip-off-0.log2534-2599.

Retain an already-open BFCP session while constructing an offer. Initial opening and every received SDP SetUpMediaSession/Open path retain normal negotiation/address updates. No guessed port is used and no wire grant is synthesized. Marker: Offer construction has a signalling-host placeholder. Integration must prove actual BFCP grant, separate decoded presentation, re-INVITE/hold/share controls and clean owned teardown.
