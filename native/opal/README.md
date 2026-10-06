# MyStudio OPAL worker

`opal_console.exe` owns both SIP and H.323 listeners and permits one active
call. Its package-local OPAL/PTLib and codec DLLs are independent of the
existing workers. JSON events use the existing schema, a process-wide sequence,
and the actual call protocol; detached recording completion retains its origin.

Build with the pinned SDK configured by the standalone build scripts:

```powershell
cmake --build <build>/native-opal --config Release --target opal_console opal_h264_tests opal_adapter_tests opal_policy_tests opal_preferences_tests opal_pcm_pacing_tests opal_pcm_stream_tests opal_video_preferences_tests opal_rtp_notifier_tests opal_content_capabilities_tests opal_shared_recording_gate_tests opal_shared_recording_tests EssaStudio_CaptureCatalog
ctest --test-dir <build>/native-opal -C Release --output-on-failure
```

Use Release consistently with the SDK import libraries. The executable directory
contains `opal64.dll`, `ptlib64.dll`, OpenSSL DLLs, and the existing x264/FFmpeg
DLLs. Its `plugins` directory contains the official `G722_ptplugin.dll` and
`mystudio_opal_aac.dll`. Startup scopes PTLib plugin discovery to that directory.
CTest adds runtime paths only to each test process; it does not change the
machine PATH. The AAC handler is registered before plugin discovery to preserve exact AAC-LC
1024-sample PCM frames.

```text
opal_console.exe --sip-port 5140 --h323-port 1760 --rtp-port 43000 [--interface IP]
protocol sip|h323
aac-enabled on|off
audio-devices <base64url microphone-ID or default> <base64url speaker-ID or default>
```

Protocol, AAC, endpoint and audio profile changes are allowed while idle. The
existing console commands control calls, identity, audio processing, camera,
desktop composition, shared surfaces and recording. Hold uses OPAL call hold;
H.323 hold changes wait for the preceding capability exchange to complete.
DTMF accepts 1â€“32 digits and sends actual OPAL tone indications in order.

G.722 and H.264 remain available with AAC disabled. SIP Cisco AAC-LD is fixed
mono 64 kbps/profile 24. H.323 also supports stereo 128/profile 25. Sony AAC-LC
uses 48 kHz stereo 192 kbps for SIP; the H.323 receive operating points include
48/32 kHz mono/stereo. H.323 LC capability matching uses explicit ASC to select
32/48 kHz; absent ASC selects 48 kHz. An omitted-ASC 32 kHz peer needs separate
device evidence. Endpoint/profile selection determines preference order,
while independent send and receive events report the codecs actually opened.
G.711 is excluded. H.264 uses negotiated dimensions, frame rate and bitrate;
text/motion preference changes IDR spacing to four/two seconds respectively.
The native default is 1920Ã—1080 at 4000 kbps. Initial SIP and H.245 capabilities
include the selected send and receive bounds; a lower peer capability limits
the negotiated stream. SIP Sony also supports H.264 packetization mode 0 using
bounded single-NAL packets; mode 1 uses FU-A where needed. Camera and desktop
requests reset off for every call.

PCM capture, microphone DSP, system audio mixing, room playback DSP and recording
share the existing media components. Network audio is resampled between codec
PCM rates and 48 kHz. PCM timestamps count exact sample frames, and source and
network receive pacing retain fractional frame duration. Recoverable LATM
startup packets keep the decoder alive until a genuine in-band mux config
arrives. Unexpected RTP audio payload types are dropped and reported before
decoding. Playback evidence counts actual nonzero decoded samples
and samples submitted to Windows, independently of call establishment. Camera
and desktop state becomes active only after capture and encoder acknowledgment.

Recording requires the exact confirmed shared screen/window ID. Its OPAL worker
consumes raw pixels from the existing selected sharing capture after outgoing
encoder acknowledgment; starting recording never opens another capture. It
records neither incoming video nor the main camera/desktop compositor. Native
recording is1920×1080, preserving the selected capture's aspect ratio. The OPAL
command is `record start <exact-source-id> mic-on/off room-on/off 1920 1080`.
Replacing, stopping or failing sharing automatically pauses the open MP4. Explicit
`record resume` requires the current screen/window to be acknowledged again;
sharing does not resume recording automatically. Call termination or explicit
`record stop` finalizes the file. The push-fed worker retains the shared recorder's bounded audio queues,
pause timeline, MP4 finalization and file rename rules.

The configured RTP base reserves its next198 ports for RTP/RTCP and the following
200–398 offsets for TCP/control transports. This gives owned workers distinct BFCP
listeners. Bases above65137 are rejected before listener setup.

SIP presentation is offered through stock OPAL SDP/BFCP sessions before a share
request; H.323 uses H.239 presentation roles. Dedicated capture begins after an
actual floor/token grant and a separate video channel opens. Failed negotiation
or a five-second grant timeout emits an explicit composed-main-video fallback.
The small reviewed stock BFCP API patch is in `patches/`. Test diagnostics expose
the emitted SDP and separately count decoded/sent presentation frames; an active
call or a grant notification alone does not establish working content media.

Account settings use the existing process environment names. SIP UDP/TCP/TLS
and disabled/optional/required SRTP are applied using compiled SDK capabilities;
unsupported policies fail visibly. Security events inspect the actual connection
and audio session. TLS uses system trust and separate local certificate files in
the user's `LOCALAPPDATA/MyStudioOpal` directory; these files are not packaged.

Local integration tests use explicit loopback interfaces and owned subprocesses:

```powershell
python tests/LoopbackWorkerTests.py --worker <staged>/opal_console.exe --protocol sip --aac off --logs <logs>
python tests/LoopbackWorkerTests.py --worker <staged>/opal_console.exe --protocol h323 --aac ld --logs <logs>
python tests/LoopbackWorkerTests.py --worker <staged>/opal_console.exe --protocol sip --aac lc --extended --logs <logs>
python tests/SipDialogFlowTests.py --worker <staged>/opal_console.exe --logs <logs>
python tests/WorkerFailureTests.py --worker <staged>/opal_console.exe --logs <logs>
```

`--extended` exercises actual desktop capture, recording pause/resume/stop,
and a second call on the other protocol while the receiver retains its old idle
selection. It retains MP4s under owned temporary folders and prints their paths
for a separate media decode probe. Camera capture requires an explicit
`--camera-id` fixture. `--probe` verifies Windows playback scoped to the owned
worker process and decodes recorded MP4 audio/video. `--debugger` uses the owned
debug wrapper; tests require clean exit and reject fatal exceptions/deadlocks.
`--duration 60` verifies sustained PCM, playback and video after hold/resume.
The failure suite checks transaction rollback, incoming device failure, listener
conflict, missing AAC DLL, the highest valid RTP/control port range and rapid
pre-grant sharing stop/repeated requests without a late capture restart. No test places a physical-device call. Synthetic
tone/card mode requires `ESSASTUDIO_TEST_MODE=1`; loopback autoanswer additionally
requires an explicit loopback interface and rejects non-loopback peers.

`--share-fixture <path>/opal_share_fixture.exe` explicitly selects its owned
cyan window from CaptureCatalog while keeping a magenta desktop decoy visible.
The `recording-pattern` MF probe verifies decoded cyan pixels and excludes the
decoy from the MP4. Inactive and mismatched recording requests are rejected
before this share starts. The fixture is closed by the integration test.

`--require-content` requires an actual BFCP/H.239 grant and separate sent/decoded
presentation frames. Extended recording also tests a second paused MP4 finalized
by hangup, actual 1920x1080 decode and fresh next-call idle state.

`--content-only --require-content --share-fixture <fixture>` performs a short
BFCP/H.239 replay with an actual grant, dedicated sent/decoded presentation,
source stop, Hold/Resume, received DTMF and clean shutdown. The full `--extended`
proof remains required for recording privacy, auto-pause and paused hangup.

SIP answer processing retains each negotiated video session role while OPAL
updates its flattened codec list, then restores the main-video capabilities.
The raw capture/receive route also reads the actual negotiated RTP session role.
This keeps dedicated presentation frames distinct from main video after a
re-INVITE and preserves encoder acknowledgments for the exact recording source.

Protocol lifecycle/readiness text and JSON share the serialized stdout writer.
PTrace diagnostics stay on stderr. `DiagnosticMarkerTests.py --worker <worker>
--logs <logs>` exercises actual SIP/H.323 calls under verbose diagnostics and
requires each established/cleared marker to remain a standalone stdout line.

The OPAL recording adapter discards a complete 20 ms audio block that arrives
behind PCM already written to MP4, including input queued before another tap
advanced the mix. It retains current microphone/room samples and the existing
pause timeline; file, encoder and genuine sink failures still terminate the job.
The actual recorder regression reproduces a microphone block at 700 ms arriving
after 760 ms was committed, then verifies pause/resume and completed MP4.

For an explicitly enabled proof, set
`ESSASTUDIO_OPAL_RECORDING_DIAGNOSTIC_DIRECTORY` to an existing absolute evidence
directory before starting the app. `opal-recording-<PID>.log` records recording
phase, generic exception or filesystem code, mixer status and sample timing.
It records no media payloads or credentials and is inactive by default.