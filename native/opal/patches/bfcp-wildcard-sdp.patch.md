BFCP SDP wildcard/control format

Pinned BFCP SDP inherited the generic application-format parser that rejects the mandatory wildcard; it also failed to initialize its media type and registered BFCP as invalid for SIP. Exact TCP/BFCP and UDP/BFCP offers yielded zero supported formats and disabled answers. Scope this fix to BFCPSDPMediaDescription: represent its wildcard as the registered BFCP control format, emit wildcard rather than an RTP payload, set its actual BFCP media type, and allow the registered control format on SIP. Unrelated application SDP and RTP format parsing remain unchanged.

Regression: opal_content_capabilities actual SDP decode/encode for both transports, plus H239 control wire-PDU test. Worker separately handles BFCP SDP session setup without trying to create an RTP media patch.
