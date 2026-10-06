BFCP TCP setup role and valid SDP port

Actual localhost SDP answer emits application port0 although the active BFCP transports were opened. The stock session uses remotePort==0 alone instead of negotiated SDP setup, creates competing Server and Participant listeners/connections, and never sets an active local SDP port. Preserve one FCS on the passive side with a separate local participant3 and negotiated remote participant ID; the active side connects only its participant to that FCS. Reuse the established correct role/socket across reoffers. An active SDP descriptor advertises valid port9, which the peer must not connect to (RFC4145 section4.1: https://www.rfc-editor.org/rfc/rfc4145#section-4.1). FromSession advertises the actual local floor-control role. UDP/TLS paths are unchanged.

Native worker allocates the manager TCP range separately after its configured RTP range so owned localhost workers have distinct passive BFCP listeners. GetLocalParticipantUserID distinguishes passive virtual participant from the negotiated remote assignment for real floor-status ownership.

Regression: two owned localhost workers SIP initial offer/answer nonzero BFCP application and correct setup roles, Hello/HelloAck, presentation FloorRequest grant/release, dedicated H264 RTP decoded by peer, restart share, hold/resume, recording1080 pause/hangup, clean shutdown under debugger.
