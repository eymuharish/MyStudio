Windows BFCP socket sentinel

Actual localhost Hello reaches the FCS, assigns the user socket, then every HelloAck fails Parameter error -4. BFCP_SOCKET is unsigned on Windows; less-than-zero never recognizes INVALID_SOCKET. Compare the exact socket sentinel while retaining the valid-socket identity check. Evidence loopback-bfcp-session-factory/sip-off-0.log678. Regression: real localhost Hello/HelloAck, floor request grant/release and dedicated presentation RTP.
