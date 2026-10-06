BFCP participant callback lifetime

Owned SIP localhost second-call teardown crashes0xc0000005 on the participant wire thread after LibParticipantWrapper destruction. The wrapper owned a live BFCP_Participant but had no destructor, so deleting the wrapper left participant/socket threads alive with a freed callback pointer. Stop the keepalive timer, disconnect/join the participant transport, and delete it while its callback owner is still alive. Apply the same release before replacing a participant on session reopen, then restart the owned timer.

Evidence: loopback-final-h239-paused-hangup/h323-off-1.log239 (participant thread20660); the MP4/media stages completed before the new SIP leg teardown failed. Regression is owned dual-worker localhost SIP connect, media, hangup/quit under debugger, requiring exit0 and no fatal exception.
