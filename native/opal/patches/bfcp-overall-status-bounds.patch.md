BFCP optional grouped status and overall bounds

Apply after bfcp-grant-fsm-parser.patch. Its newly restored per-floor parser previously required a REQUEST_STATUS, while the existing overall helper dereferenced a missing status and read nested children beyond its group. The owned private-parser fixture reproduces 0xc0000005 (evidence/bfcp-overall-only-red.log).

[RFC8855 sections5.2.17-18](https://www.rfc-editor.org/rfc/rfc8855.html#name-floor-request-status) make REQUEST_STATUS optional in both groups. Preserve a received floor ID with NULL status; do not substitute an assumed status or floor. Parse the overall group with the same explicit message/parent bounds and duplicate/unknown-status rejection, freeing temporary values on failure. Participant consumers check optional pointers and may use an explicitly received per-floor status when the overall status is absent.

The existing opal_bfcp_nested_status CTest now covers explicit Granted, header-only floor with explicit overall Granted, no-status overall/floor remaining NULL, truncation/cross-group/unknown/duplicate status rejection. GREEN is saved in evidence/bfcp-status-fsm-green.log. The distinctive source marker is MyStudio bounded optional overall status.
