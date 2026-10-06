Established SIP dialog controls

Apply after sip-dialog-bye-flow.patch. Re-INVITE and INFO also lacked an explicit transport and attempted to connect to the peer's closed Contact port. The common transaction constructor now consults the guarded established reliable flow only when its caller supplied no transport. The guard requires ConnectedPhase or later, an open reliable transport, and the unchanged next-hop IP/protocol. Initial INVITE, UDP, explicit transports, closed flows and changed routes retain stock lookup. TLS validation is unchanged.

Owned localhost regression SipDialogFlowTests.py responds to hold/resume re-INVITEs, INFO and BYE on the original TCP dialog, advertising a closed Contact. Requires each200 response, INVITE ACKs and clean worker exit. Root physical logs require the same trace proof and remote participant removal.
