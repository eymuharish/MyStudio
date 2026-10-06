Windows BFCP thread lifetime

The wire thread clears running/connected flags before its final Log callback. Flags cannot establish that the callback owner is safe to delete. After signaling socket closure, wait and close the actual owned Windows wire and retransmission handles; avoid self-join and clear already-closed forced-stop timer handles. Participant timer Stop is handled by the separate wrapper lifetime patch.

Regression: owned localhost TCP BFCP connect/floor/share/release plus hangup/quit under debugger; require clean exit0 and no late callback exception.
