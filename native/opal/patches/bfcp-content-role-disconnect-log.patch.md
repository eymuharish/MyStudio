Incoming BFCP presentation mapping and disconnect log

Owned TCP BFCP now negotiates nonzero SDP and exchanges Hello/HelloAck. Actual floor2 request reaches receiver but receives InvalidFloor: receiver slides session uses video media type with presentation contentRole, while stock mapping looks only for the presentation media type and incorrectly registers main floor1. Select the mapped actual session by either presentation media type or explicit negotiated presentation contentRole. Use the same rule for requests.

The teardown crash is a separate exact varargs defect: OnBFCPDisconnected logs `%d %s %d` with only one socket argument. Replace it with one correctly typed socket placeholder. Thread3300 crashed immediately after the actual socket-aborted message; ownership/thread joins cannot protect against an invalid printf argument.

Evidence: loopback-bfcp-role-fixed/sip-off-1.log2863 (Floor2 does not exist), sip-off-0.log2892 (0xc0000005). Required regression: actual localhost floor grant, separate peer decoded presentation frames, stop/restart, paused1080 recording/hangup and clean debugger shutdown.
