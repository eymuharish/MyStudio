# Pinned OPAL compatibility patches



`bfcp-floor-control.patch` exposes the existing participant's request/release methods through `BFCPSession`. Application code waits for the real granted/aborted callback before starting dedicated presentation media. A successful send only means the request was queued.



The same patch corrects the UDP virtual participant start check. `BFCP_Participant::OpenTcpConnection` returns `bool`; its implementation sets `Status=true` when `connect()` succeeds. The TCP branch already rejects `!OpenTcpConnection`, whereas the UDP branch rejects success. The patch aligns the two checks. Require BFCP grant/release and dedicated RTP integration tests before marking presentation verified.



`bfcp-presentation-floor.patch` selects the dedicated presentation floor. `bfcp-wildcard-sdp.patch` represents BFCP as a supported control format, accepts its `*` application token and omits RTP codec attributes. It does not enable unsupported media formats.



Apply `bfcp-negotiated-presentation.patch` after those patches. It initializes participant/server with the negotiated presentation floor, registers their users before callbacks and reuses the existing control session in SDP construction.



`bfcp-request-list-ownership.patch` makes the FloorRequest wrapper own its request list through validation, message construction and sending. The lower overload borrows it. A failed request without a transport must report failure and exit cleanly; the probe tests this repeatedly. FloorQuery ownership is unchanged.



`bfcp-participant-lifetime.patch` stops participant keepalive and wire callbacks before deleting their owner, including session reopen. Real presentation/teardown tests verify orderly child shutdown.



Apply `bfcp-windows-socket-sentinel.patch`, `bfcp-windows-thread-join.patch` and `bfcp-tcp-role-port.patch` after participant lifetime. They recognize Windows unsigned INVALID_SOCKET, join the actual owned transport/timer threads before owner deletion, and honor negotiated TCP setup with one passive FCS plus separate active participants. TCP active SDP uses valid port9 per [RFC4145 section4.1](https://www.rfc-editor.org/rfc/rfc4145#section-4.1); its peer retains the passive listener rather than connecting to that placeholder. Adjacent `.patch.md` files identify the failing source/evidence and required actual wire/media regressions. Floor grants and separately decoded presentation frames must pass before declaring BFCP verified.



`bfcp-content-role-disconnect-log.patch` follows TCP role/port. It recognizes incoming video sessions with negotiated presentation contentRole when registering/requesting the actual floor, and fixes the disconnect callback log's missing variadic arguments. Owned wire logs demonstrate Hello/HelloAck and floor2 arrival before this correction; complete grant/decoded-content/clean-teardown evidence is required after it.



`bfcp-grant-fsm-parser.patch` follows content-role/log: request and release use the existing FSM actions, and the mandatory nested floor status parser retains the actual granted floor. Only a valid received grant starts content.



`rtp-notifier-lifetime.patch` removes callbacks by their bound target. `bfcp-logger-lifetime.patch` keeps the shared logger alive through server/participant teardown.



Apply `sip-dialog-bye-flow.patch` before `sip-dialog-controls-flow.patch`. These preserve the successful reliable dialog transport for BYE, hold/resume re-INVITE and INFO when a peer advertises a closed ephemeral Contact port. Reuse requires an open reliable transport and the same next-hop address/protocol. Initial calls, UDP and changed next-hop addresses retain the existing lookup. Owned TCP regression fixtures verify peer200 responses and clean termination.



`scripts/Build-OpalDependencies.ps1` applies the patches to the pinned SDK in this order and records the build. Recompile all header consumers and relink against the resulting matching import library.


`bfcp-overall-status-bounds.patch` follows grant-fsm-parser: optional per-floor/overall statuses remain NULL when absent, overall children stay within their group, and participant callbacks use only an explicitly received status with the actual floor ID. `bfcp-local-fsm-event-safety.patch` then initializes local control context and bounds NULL/success logging. The same private-parser/FSM CTest covers both owned crash reproductions and valid optional wire forms.

`bfcp-offer-session-reuse.patch` preserves an already-open control session while constructing an SDP offer, whose signalling-host address carries no negotiated BFCP port. Received SDP still updates the session normally. `bfcp-failed-connect-join.patch` uses the existing Windows close/join owner after connection failure, avoiding a second close of the transport handle. Both have owned real-wire/crash evidence and focused rationale files.

`bfcp-session-open-state.patch` follows the two offer/failed-connect patches: BFCP reports its real owned socket lifecycle instead of the unattached generic media transport, and Close stops/joins both wrappers.

`sip-idle-read-timeout.patch` retries only reliable transport idle timeout before the first PDU byte; actual EOF/reset and partial-PDU limits remain unchanged. `SipDialogFlowTests.py --idle-seconds 90` verifies the same accepted TCP flow survives the quiet gap and receives BYE200 despite a closed ephemeral Contact.
