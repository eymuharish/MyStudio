# Opt-in physical studio transition verification

This mode makes physical calls. Run only when explicitly authorized and using a fresh isolated data root and evidence folder. It runs the actual WinUI studio-selection, protocol-selection, dial, sharing, audio, recording, hangup and shutdown handlers. It does not change remote device settings.

```powershell
MyStudio.exe --studio-transition-smoke <fresh-evidence-folder> --data-root <isolated-data-folder> --transition-calls 8 --transition-share-fixture <ShareFixture.exe> --transition-playback-probe <MediaProbe.exe>
```

The default is four calls; only four or eight are accepted. Calls alternate Cisco SX20 at `10.136.51.25` using SIP and Sony at `10.136.51.11` using H.323. The explicit optional `--transition-sony-address 10.136.61.31` selects the authorized alternate Sony. Each connected call lasts 45 seconds; each complete call phase has a 150-second deadline. Connection confirmation is bounded 45 seconds, and each cleanup keeps an independent 25-second OPAL termination/finalization budget. A new owned sharing fixture is created for each call and expires after 240 seconds even if cleanup fails.

Proof requires actual microphone capture, confirmed microphone/room/system audio states, fresh activation of the exact owned fixture HWND with sharing restarts, paused recording time remaining frozen, final-window audio/video counter growth, a fresh Windows process playback probe, and finalized MP4 decoding with the owned cyan pattern. Negotiated audio directions are compared within and across calls for the same target/protocol; RTP payload numbers are not compared.

Media-only failures such as a quiet room microphone are recorded as `MEDIA_FAIL` and allow a controlled, confirmed hangup before the next call. Command errors, timeouts, worker lifetime errors and cleanup failures stop the run. The overall report remains `FAIL` if any call fails any gate. Each call records events, screenshot, playback/decode reports and independent failure reasons.

Resource samples contain process PID/start time, threads, handles, private bytes, working set and total CPU time at selection, connection, active media, stable dwell, settled post-hangup and final disposal. A family switch must retire the actual previous process identity before replacement. Samples describe observed resource trends; they do not establish absence of long-term leaks or remote physical audibility/display.

Capture catalog refresh has a 5-second deadline and at most two pending requests. Disposal waits at most 2 seconds; if native enumeration is still running, its background thread retains the native API/DLL and unloads it only after the query exits. That is a bounded caller return, not a forced native thread termination. Normal command gates/pipes have 8-second deadlines; OPAL recording/BYE shutdown retains 25 seconds and keeps stdout/stderr draining.
