BFCP grant FSM and mandatory nested status parser

Owned actual TCP peer now accepts floor2, sends Granted FloorRequestStatus, and sender parses/ACKs it, but cannot notify the worker: RequestFloor bypasses the normal FloorRequest FSM action and leaves state Connected, where grant is a no-op. Route request/release through existing FSM actions. Request queuing never activates media.

UDP initialization now uses the same actual presentation content-role helper as TCP, so the participant FSM requests the selected negotiated presentation floor in either transport.

FLOOR_REQUEST_INFORMATION parsing explicitly disabled the mandatory nested FLOOR_REQUEST_STATUS under #if0. Restore parsing with message and parent-group bounds. The nested parser stops at its group boundary, rejects missing/duplicate/invalid request status and zero floor IDs, and frees temporary allocations. Missing mandatory floor status remains rejected. No floor ID or grant is inferred from overall status.

Evidence: loopback-bfcp-role-floor-fixed/sip-off-0.log, lines2458-2461: Granted received/acked then NoAction with floorID0; current teardown is clean. opal_bfcp_nested_status fails against the preceding source with "Valid nested Granted floor was lost" (bfcp-nested-status-red.log). It exercises the genuine grouped Granted packet plus truncated, missing, cross-boundary, zero-length, unknown-status and zero-floor cases. Integration still requires actual wire Granted and separate decoded presentation video, floor release/re-request, shared-source recording1080 pause/hangup, clean owned process exit.
