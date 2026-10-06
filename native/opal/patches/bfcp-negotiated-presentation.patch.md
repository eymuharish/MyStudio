BFCP negotiated presentation initialization

Owned actual floor request trace proves RequestFloor asks presentation floor2 while stock participant/server contain only first main floor1, so the request is rejected before any BFCP wire message. Initialize both using the actual presentation session's negotiated floor mapping, and register the negotiated remote user and virtual local user before callbacks. SDP session creation must reuse an existing BFCP session rather than adding a duplicate; resize its session flags before indexing a newly appended session. Worker supplies BFCP as a supported SIP control format and handles control SDP sessions without an RTP media stream.

Apply after bfcp-wildcard-sdp.patch and existing floor API patches. Regression is actual owned SIP --extended --require-content, demanding floor grant and separately decoded presentation frames. Keep failures until this real control/media proof passes.
