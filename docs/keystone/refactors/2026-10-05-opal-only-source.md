# OPAL-only current source

Goal: remove the obsolete PJSIP, H323Plus and Liblinphone implementations and launch paths from this OPAL edition. Earlier release trees, archives, logs, recordings and user settings remain intact.

Invariants: MyStudio WinUI remains the host; OPAL keeps SIP and H.323, camera/display/window sharing, room and system audio, AAC/G.722 negotiation, recording, shared surfaces, diagnostics, English and selectable Arabic. Preserve settings fields when migrating old backend tokens to OPAL (numeric value 2).

Dependencies: move the three command-parser files from native/linphone to native/opal before deleting old native folders. Move WPF MainWindow.CopySettings to an engine-neutral settings helper before deleting the WPF host. Retain the engine command/event/recording boundary, OPAL worker, shared native/media components and current WinUI. Remove obsolete engine factories, selection branches, executable paths, installer and old engine-specific scripts/tests/docs. Remove unreferenced prototype App/Client/Engine projects.

Slices: parser move and native cleanup; settings helper and migration characterization tests; managed host/factory removal; obsolete verification artifacts; current solution and WinUI builds; remaining managed/native tests; independent review and package UI smoke.

Isolation: current source repository only, codex/opal-integration at 2255dfb. Existing dirty paths belong to the authorized current work. Root owns active OPAL/SDK/AAC/media changes; shared files require narrow coordinated edits. Resolve and check all deletion paths against the absolute source root, using PowerShell native operations. Tracked baseline files can be restored from 2255dfb; never reset the current root-owned changes.

Proof: pre-cleanup managed suite passed 708 tests and Release x64 WinUI build passed. Settings migration must first fail against old backend behavior, then pass preserving the document and unrelated settings. Build current WinUI and run the remaining managed suite. Root owns native build, current OPAL tests, package and standalone call-bar smoke.

Rollback: recover deleted tracked paths individually from commit 2255dfb after reviewing current ownership; retain current OPAL edits. Review focus: hidden old engine launch paths, inherited command behavior, settings loss, source build references, package manifest and tested behavior gaps. Removed old engine tests cease to represent supported behavior and are not counted as regression coverage.

Completed source proof: Release x64 WinUI build passed with zero warnings/errors; 473 remaining desktop tests, 78 settings tests and 17 contract tests passed. The 24 core in-call audio viewmodel tests remain. Removed source-string/WPF surface/layout tests represented retired UI, while current WinUI has real bilingual call-bar fixture QA. Root owns final fresh package QA and native proof.

Current VPN requirement: remove the LAN selector and option from WinUI entirely, use external VPN at startup and every family/protocol change, and migrate old building/lan settings on load without rewriting the original document or losing names, devices, records or history. Compatibility enum/tokens remain only for deserialization. Video migration marker is copied, serialized and compared to avoid resetting saved preferences every launch. The historical libyuv origin remains in provenance; standalone media CMake now accepts explicit verified libyuv artifacts instead of paths into PJSIP.

Deletion safety: automatic approval rejected the initial broad recursive source deletion. Recoverable explicit tracked-file patches removed obsolete text instead; only two explicitly named partial archives were removed separately. Historical release trees and logs were untouched. Original branding icons and ignored local build output remain unreferenced and preserved.
