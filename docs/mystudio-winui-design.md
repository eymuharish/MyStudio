# MyStudio WinUI 3 design and implementation brief

Historical implementation brief, superseded for the OPAL edition on 2026-10-05. Current behavior and verification are recorded in `opal-validation.md`: OPAL is the only engine, AAC stereo is preferred with compatible mono/G.722 fallback, the network profile is VPN only, studio cards show names and tooltip icons, and the portable layout is `MyStudio.exe` beside `appfiles`. The earlier test deadline below applies only to its original session; subsequent physical calls require current user authorization.

The user requests a real WinUI 3 / Fluent interface and renames the application MyStudio. The purpose remains dependable single-device Cisco/Sony studio calling over LAN and routed VPN, with useful audio, camera, presentation and recording tools. This request expands the existing integration; it does not replace its compatibility work. The user explicitly instructs continued implementation without waiting for replies, so routine architecture/design choices below proceed within that authorization.

## Interface

- Actual Microsoft.UI.Xaml application, unpackaged portable x64 evaluation, Windows App SDK 2.5.1 stable (Microsoft release channels and NuGet index verified live). No WPF reskin presented as WinUI 3.
- Fluent NavigationView: studios, direct call, active call, settings, diagnostics. Use clear text alongside standard icons, native Fluent controls, rounded cards, sensible spacing, keyboard access and visible focus.
- Arabic and English with correct RTL/LTR direction. System/light/dark themes; Mica when supported with a normal opaque fallback. Resize to laptop screens without hiding call controls.
- Studio cards show saved model/address/protocol and actual availability only when measured. Direct calling exposes endpoint profile, SIP/H.323, SIP backend and routed network mode.
- During a call, moving remote video is primary, local camera preview is secondary, and the controls cover answer/reject/end, microphone/room mute, camera, screen, recording, audio settings, Hold/Resume and DTMF. Disable unsupported controls from real capabilities.
- Show separate BFCP/H.239 state and clearly labeled main-video fallback. Never claim a second channel from ordinary composition.
- Settings include camera/display selectors, overlay, resolution/bitrate, microphone selection, noise reduction, gains/EQ/system audio, recording folder, optional account/TLS/SRTP. Diagnostics show actual negotiated G.722/AAC, transport evidence and unknown values honestly.

## Architecture and preservation

Reuse tested contracts, presentation/settings code and native console boundary. Keep the WPF desktop source as a compatibility reference and backend assembly where useful; move shared logic only when needed to keep the WinUI application functional. Use a real WinUI video presenter for existing shared D3D surfaces, with measured CPU fallback if GPU composition is unavailable. Do not open WPF dialogs from the normal MyStudio flow as a substitute for native WinUI dialogs.

MyStudio is the user-facing identity and new executable. Existing internal TeleStudio namespaces and native protocol names need not change. Preserve protected previous releases, installed EssaStudio, records, credentials and production configuration. Start-Test.cmd points to isolated TestData; no installer or production replacement is authorized here.

## Order and verification

1. Complete pending audio/recording regressions: real negotiated codec event, repeated recordings and redial, independent MP4 audio/video decode with known signal and moving test card.
2. Scaffold actual WinUI application and dependency restore; build and launch a minimal window before expanding controls.
3. Connect existing backend/settings, studio directory and all call/media tools; implement native WinUI settings/security/diagnostic dialogs and real video surfaces.
4. Verify Arabic/English, light/dark, laptop dimensions, keyboard controls, real local media pipeline and recording. Capture actual running screenshots. Build the self-contained portable MyStudio package and verify relocation/dependencies.
5. Fresh review and fix findings. Report measured compatibility separately from missing EX90/LAN access and unobserved physical audibility.

G.711 is forbidden. G.722 is the Liblinphone audio codec; existing supported AAC options remain in legacy paths. Studio test calls must end before Sunday 2026-10-04 06:55 Saudi time. No new five-minute studio call after 06:49; graceful stop begins at 06:54 and a task-scoped watchdog terminates remaining physical workers at 06:54:30. Local loopback/UI/software work may continue afterward.
