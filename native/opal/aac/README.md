# MyStudio OPAL AAC plugin

`mystudio_opal_aac.dll` exports `PWLibPlugin_GetAPIVersion` (1) and
`OpalCodecPlugin_GetCodecs` (codec API 7). It uses the header from the matched
`OPAL_SOURCE_DIR`; codec API requests older than 7 return no formats.
FDK-AAC 2.0.3 is linked statically from the verified isolated install, so no
FDK runtime DLL is required. Ship its `licenses/fdk-aac-NOTICE` with the package.

| Format | Protocol | PCM | Bitrate | RTP clock | PCM samples per packet |
|---|---|---|---|---|---|
| MyStudio-AAC-LD-LATM-64 | SIP, H.323 | 48 kHz mono | 64 kbps | 90 kHz | 960 |
| MyStudio-AAC-LD-LATM-128 | SIP, H.323 | 48 kHz stereo | 128 kbps | 90 kHz | 960 |
| MyStudio-AAC-LC-48k-stereo192 | SIP, H.323 | 48 kHz stereo | 192 kbps | 48 kHz | 1024 |
| MyStudio-AAC-LC-48k-mono96 | SIP, H.323 | 48 kHz mono | 96 kbps | 48 kHz | 1024 |
| MyStudio-AAC-LC-32k-stereo192 | H.323 | 32 kHz stereo | 192 kbps | 32 kHz | 1024 |
| MyStudio-AAC-LC-32k-mono96 | H.323 | 32 kHz mono | 96 kbps | 32 kHz | 1024 |

Each format has separate encoder and decoder definitions and instances. The
32 kHz LC formats represent existing Sony receive operating points; they do not
add new UI choices. The application chooses formats by endpoint family and
protocol, gates AAC changes to idle, defaults AAC on when the plugin is available,
and prefers stereo before the matching mono operating point, then G.722 fallback.

SIP mono AAC-LD requires `profile-level-id=24;object=23;bitrate=64000` and in-band
configuration. `cpresent=1` may be explicit or omitted: [RFC 6416 section7.3](https://www.rfc-editor.org/rfc/rfc6416.html#section-7.3)
defines the omitted default as1. The SX20 TC7.3.3 answer omits it, and the
regression reproduces that answer while retaining RTP99/90000 and mono PCM48k.
Explicit `cpresent=0` remains unsupported and rejected.
The 128 kbps/profile25 stereo format supports both protocols, but cannot select
the distinct fixed64/profile24 mono encoder. SIP AAC-LC uses MPEG4-GENERIC/AAC-hbr with
`streamtype=5;profile-level-id=15;config=1190;sizelength=13;indexlength=3;indexdeltalength=3`
for stereo; mono uses its matching `config=1188`. Negotiated transport stereo does
not imply that the current mono microphone/loopback mixer captures two independent
input channels.
Set the standard `Protocol` media option (`SIP` or `H.323`) before transcoding.
Stamp master, local/raw, and network media formats with the active protocol:
the core's `Protocol` option uses `NoMerge`, so an initially empty master does
not inherit SIP/H.323 from the remote capability. AAC rejects that unspecified
protocol rather than guessing whether Sony LC has SIP AU headers or H.323 raw data.
SIP and H.323 LATM carry FDK's AudioMuxElement directly at the first RTP payload
byte, as required by [RFC6416 section6.1](https://www.rfc-editor.org/rfc/rfc6416.html#section-6.1)
and used by the existing Cisco SIP factory. No additional LATM length prefix is
added or removed. SIP LC uses RFC3640 AU headers; H.323 LC uses raw access units.
The send stream must mark complete AAC packets with M=1.

A receiver joining between LATM StreamMuxConfig repetitions, including a new
decoder after hold, consumes FDK transport-sync/missing-bit packets without
producing PCM and retains its decoder state. It resumes real PCM after an
in-band configuration arrives; it never guesses the remote mux layout.
The core regression loses initial packets and reopens the decoder against the
same running encoder for LD64 SIP/H.323 and LD128 H.323. Malformed SIP outer
LC AU framing, unsupported AAC objects/rates/frame lengths, and invalid options still
fail. `ESSASTUDIO_DIAGNOSTIC_LOG=1` enables at most eight waiting/error messages
per decoder; these contain numeric codec metadata, not payload contents.

All H.245 formats use MPEG4 audio OID `0.0.8.245.1.1.0`. Non-collapsing options
include profile ordinal 0 and AOT ordinal 3. LATM adds logical ordinal 1,
collapsing AL-SDU frames ordinal 2 (2), audio objects ordinal 5 (1), and mux
configuration ordinal 6 in OLC only. LC adds ASC octets ordinal 4 in OLC and
RequestMode and AL-SDU frames ordinal 2 (1). ASC determines sample rate and layout.

Normal `Clock Rate` and `Frame Time` options explicitly describe transport ticks:
LD 90000/1800, LC rate/1024. Definition sample rates retain the raw PCM rate.
This handles the matched OPAL code's unused separate `RTP Clock Rate` option
and avoids its microsecond-to-tick rounding of 1024/48000.
Clock, frame, and fixed AAC target bitrate metadata use `NoMerge`: OPAL codec selection merges PCM and
encoded formats, which must keep separate 48 kHz PCM and 90 kHz LD transport
units. SDP rtpmap matching and strict codec options still reject invalid clocks.
The decoder selection must retain the supported encoded bitrate instead of
copying the uncompressed PCM target rate (for example 768000 into AAC-LD64000).

Standalone build:

```powershell
cmake -S source/native/opal/aac -B build/aac -G "Visual Studio 18 2026" -A x64
cmake --build build/aac --config Release
ctest --test-dir build/aac -C Release --output-on-failure
```

`OPAL_SOURCE_DIR` defaults to `dependencies/opal`; `TELESTUDIO_AAC_LD_INSTALL`
defaults to `dependencies/media/aac-ld`. No OPAL/PTLib linking is needed for this
DLL or `opal_aac_plugin_tests`. That test checks standard ABI load, all eight
protocol/format PCM roundtrips, independent instances, output/input bounds,
malformed payloads/options, and unsupported operating points.

When the parent provides `opal_sdk`, `opal_aac_core_tests` also builds. It checks
real OPAL registration, generated SDP, exact raw frame sizes, and PCM conversion
through OPAL transcoders. Real SDP encode/decode must retain both supported
formats and create codecs from the parsed options. Regression cases reject
Cisco profile25/object2/128kbps/cpresent0 and Sony AAC-lbr/mismatched ASC.
An independent wire-contract test compares raw H.323 FDK output with SIP LATM
payload bytes and decodes each through the opposite protocol, including Cisco's
M=0 input. A SIP-only roundtrip cannot detect a matching extra wrapper on both ends.
Every core roundtrip selects both encoder and decoder through
`OpalTranscoder::SelectFormats`, including its real PCM/encoded format merges.
The selection regression proves that an unstamped master is rejected, while
stamped formats retain their protocol and fixed target bitrate in both directions.
Options without FMTP names have no FMTP defaults, so SDP cannot reset H.245
metadata. The single AAC-hbr mode uses an enum because OPAL concatenates parsed
FMTP values with existing string option values.

Link the worker with `opal_aac_handler` and call
`RegisterMyStudioOpalAacHandler()` before PProcess/plugin initialization. This
scoped handler restores exact AAC frame sizes: stock OPAL's integer microsecond
LCM requested 4,095,936 bytes for a 48 kHz stereo LC frame requiring 4096 bytes.
It leaves the plugin ABI and upstream core sources unchanged. The handler factory
returns fresh instances because OPAL deletes them after registration.
After plugin loading and before constructing H.323 endpoints, call
`RegisterMyStudioOpalAacCapabilities()`. Its scoped LC capability overrides
stock generic matching: TCS excludes ASC, so identical 32/48 kHz operating
points select the 48 kHz default; an OLC carrying ASC selects the exact rate and
layout before the core commits to its first capability match. Explicit 32 kHz
ASC remains supported. Legacy 32 kHz peers omitting ASC are not verified.
The real TCS/OLC regression puts 32 kHz first, requires default48 TCS plus exact
32/48 OLC matching for mono/stereo, and rejects an unsupported ASC.

Verified with matched OPAL 4.0 commit `aeda428ced0b1d8c96d4290bea97362bceb7faf2`
and VS2022 v143: all eight core PCM roundtrips, exact sizes, generated SIP SDP,
and H.245 ordinal ordering/profile25/object2 rejection. AAC-LD raw input is
1920/3840 bytes mono/stereo; AAC-LC is 2048/4096 bytes at both 32 and 48 kHz.
Core tests explicitly disable automatic plugin directory scanning and load the
tested DLL. CTest adds the parent app-local deployment and dependency runtime
directories to the core test's PATH. Direct execution of its EXE requires
OPAL/PTLib and OpenSSL runtime directories on PATH.

Passing the standalone test alone does not verify
core SDP negotiation, physical device calls, or played audio.

## Rebuilding the OpenSSL dependency

`BuildOpenSsl.ps1` clones the official `openssl-3.5.9` tag at commit
`45e844fa2a14ec92d146bd8f5778ac130b6625fb`, builds Windows x64 shared libraries
with VS2022 v143, and installs into the isolated dependency prefix. Provide a
native Windows Perl executable; MSYS Perl is unsuitable for this target. Official
portable Strawberry Perl can be extracted in `build/openssl/strawberry` without
installation. The verified 5.42.3.1 portable ZIP SHA-256 is
`6a081a811781c30aca51dbc036afd93092af91e3297901f02c17043795a10690`.

```powershell
./source/native/opal/aac/BuildOpenSsl.ps1 `
  -PerlExe ./build/openssl/strawberry/perl/bin/perl.exe `
  -OutputRoot .
```

The script sets the VS environment inside its private command process and builds
`VC-WIN64A shared no-tests no-asm no-makedepend`. Generated Makefile compiler
flags use `/Z7` instead of `/Zi` for embedded debug information, avoiding the
compiler PDB locking reproduced during parallel dependency builds. This changes
build files only; the official OpenSSL source remains unchanged. Logs go under
`evidence/openssl-*`; a pre-change Makefile is retained in `build/openssl`.

PTLib uses `dependencies/openssl/include/openssl` plus the import library copies
at `dependencies/openssl/lib/VC/x64/MD/{libcrypto,libssl}.lib`. Runtime DLLs and
`openssl.exe` remain in `dependencies/openssl/bin`; configuration uses the local
`dependencies/openssl/ssl` path. Include `dependencies/openssl/LICENSE.txt` when
shipping. Official source: https://github.com/openssl/openssl/tree/openssl-3.5.9.
