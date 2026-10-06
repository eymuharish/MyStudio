SIP BYE established reliable dialog flow

Owned closed-Contact localhost regression and EX90 trace show BYE reconstructing a TCP connection to the Contact ephemeral port, which refuses connections, producing no-transport instead of a BYE. Preserve the successful INVITE transaction/inbound INVITE reliable transport on the connection and pass it to BYE. Reuse is allowed only while open, reliable, with the same resolved next-hop IP and transport protocol; route changes and UDP retain stock lookup. TLS continues on the authenticated existing connection; this patch does not change authentication, cipher or certificate validation.

Apply with git apply --recount; rebuild OPAL DLL/import library and dependent worker because SIPConnection class layout changes.
