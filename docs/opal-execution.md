# MyStudio OPAL execution ledger

Approved plan: independent Windows x64 MyStudio with OPAL SIP/H.323, existing media/recording tools, compact-control changes, and optional AAC-LD/Cisco and AAC-LC/Sony codec plugin. AAC is configured while idle, defaults off, and falls back to G.722; G.711 is excluded. Physical device tests are newly authorized after local checks, with bounded sessions and owned-process cleanup.

Protected: original liblinphone source and all prior release/install/data files. Isolation: new clone on codex/opal-integration under outputs/opal-20261004/source. Original source was clean at d50caf6e14c9c56954e0584aa129243cfdbf1550. Eight approved UI files imported from the isolated compact-controls source.

## Contracts

- Native worker: Engines/Opal/NativeEngines/opal_console.exe --sip-port 5140 --h323-port 1760 --rtp-port 43000 [--interface IP]. One process and both listeners; one active call.
- Existing stdin command vocabulary and ConsoleEventWriter JSON protocol remain stable. `protocol sip|h323` selects the command protocol while idle. All JSON events carry the actual protocol and monotonically increasing sequence. New command: `aac-enabled on|off`, rejected during a call.
- Managed integration uses a shared worker behind two IProtocolConsole views and an OpalEngineCommandSink that retains the existing event validation, recording, and surface acknowledgment behavior. Legacy factories remain unchanged for regression tests.
- AAC plugin: native/opal/aac, OPAL codec API from the pinned OPAL tree, FDK-AAC wrappers from native/media. Export normal OPAL plugin entry points; build encoder/decoder pairs with SIP and H.245 options. Include a load-and-roundtrip test, unsupported negotiation tests, and independent encoder/decoder contexts.

## Tasks

- [x] Isolate source and import compact controls.
- [x] Build pinned PTLib and OPAL with compatible Windows toolchain (matched x64 DLLs/import libraries and G.722 plugin).
- [x] Implement native protocol/media worker and regressions.
- [x] Implement managed shared worker, AAC controls, identity/isolation and regressions (696 tests, self-contained publish and LAN/VPN UI smoke cases).
- [x] Build AAC plugin and verify negotiation/roundtrips.
- [x] Validate local real media, recording and package relocation.
- [x] Run bounded physical Sony/Cisco matrix and capture actual decoded media evidence, with studio034 audio-quality caveat.
- [x] Independently review the managed/native changes and stage the portable package with matching hashes and native PE closure.

Ruling: this conversation's approved plan is the design/spec authority; no additional approval loop is required. Existing old physical-test cutoffs belong to the old candidate; new test sessions use current scoped deadlines and cleanup.

Ruling: official v3_18 source has no BFCP implementation, contrary to the planning assumption. Official master provides native BFCP session support. Use matched current OPAL/PTLib commits (OPAL aeda428ced0b1d8c96d4290bea97362bceb7faf2; PTLib c2e9dea2434871baf3f9039db1aadb151bbd7c8b), compile the codec plugin against that exact header and preserve the original branch clones as dependency exploration evidence. This increases API adaptation work but satisfies the required dedicated sharing channel.

Verified: PTLib4 x64 DLL/import library built with app-local OpenSSL3.5.9. Rebuild adaptations are recorded in scripts/Build-OpalDependencies.ps1 (Windows SDK10 DNS/IPv6 detection, missing AWS PProcess include, disabled Deepgram feature guard, version fallback pointer, OpenSSL header aliases and MergeSym C++17). Compiler /FS is scoped to build children; no shared compiler processes are terminated.

Physical targets: SX20 10.136.51.25, Sony XG77 10.136.51.11, XG80 10.136.203.16; user supplied EX90 10.136.101.18 and 10.136.101.12. All five identities were confirmed through authenticated web UI. All20 outbound protocol/AAC combinations were exercised;16 passed local media checks and four XG77 cases remain limited by near-silent incoming audio. Device configurations were unchanged.

User amendments: camera sharing starts off on every new call and is available only through explicit Share from source selection, alongside monitors/windows. Tests focus on calls initiated by MyStudio to Sony/Cisco; local accepting endpoints are test fixtures. Fresh Cisco SX20/EX90 profiles use FHD 1920x1080 at 4000kbps and matching H.264 negotiation; preserve existing explicit resolution choices and Sony defaults.

Further amendments: delete the messaging/DTMF row; recording actions are accessible icons in the floating frame only. OPAL recording is fixed1920x1080 and consumes the already selected, acknowledged screen/window feed. It never opens an independent desktop capture. Sharing stop/change/failure pauses the open recording; a fresh acknowledged screen/window and explicit Resume are required. Camera or absent sharing cannot resume recording. Explicit recording Stop and call teardown finalize the file. One global LAN/VPN setting, OPALData/connection-environment.json, applies at startup and across every device/protocol/profile switch; editing is idle-only.

Current real proof: selected cyan window recording decoded at1920x1080 with zero excluded magenta-window pixels; autoPause/re-share/manualResume and actual separate BFCP/H239 presentation frames passed locally. Merged WinUI media, compact floating controls, global mode settings, MP4 decoding and18 UI cases passed on staged15. The final native marker build passed15 regression tests. A90-second physical EX90 SIP AAC-LD call and90-second owned TCP media fixture both proved in-dialog commands and BYE200; genuine EOF/partial-PDU failures still clear. Prior shutdown, BYE and diagnostic-marker failures remain in evidence, superseded by the matched corrected runs. SonyXG77 incoming audio remained near silence and did not pass the independent Windows playback threshold. Local media and peer receiver/termination evidence are separate claims.

Final amendments: studio034 is10.136.51.11,032 is10.136.203.16 and018 is10.136.61.31. Known-device support data selects the tested profile/protocol/G.722 and excludes unsupported protocols/codecs/presets in the UI and before dialing. Studio018 is H.323 only;032 AAC fell back to G.722 and is unavailable. Studio034 displays its near-silent microphone caveat. Canonical physical results are in evidence/studio-support-evidence.json; final source catalog contains only verified stable protocol/codec combinations.

Actual final selection checks:018 then SX20 selected the expected model/protocol/G.722, with one initialization per family switch and VPN preserved. Default-port LAN startup passed; thirteen bilingual catalog checks and18 layouts passed. A late microphone block60ms behind committed recording PCM reproduced SinkFailed; the OPAL-only committed-watermark guard then passed the actual recorder regression and independent1080p/nonzero-audio decoding. Genuine sink errors remain fatal; the legacy mixer is untouched.

Final merged replay on staged19:437 actual WinUI frames,695 decoded peer frames, compact call/share/recording controls, frozen pause time,18 layouts and13 studio checks passed. Final MP4 decoded1920x1080 with40 video frames and209802 nonzero PCM values; GUI exit0. Expanded native15/15 tests retain genuine encoder and filesystem failures. Delivery ZIP, source archive, checksum manifest and desktop shortcut are generated from this reviewed committed edition.

First physical outbound SX20 SIP/G.722 run sustained60seconds. Authenticated receiver Call details showed H.2641280x720 at21fps. Native decoded nonzero audio and independent Windows included-process playback passed. Receiver sent no video in this run. The run is FAILED because teardown exited0xc0000409; keep it as failure evidence and rerun after correction at requested1080p.
