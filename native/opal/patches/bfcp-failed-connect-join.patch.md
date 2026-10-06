BFCP Windows failed-connect handle ownership

The failed-connect branch calls BFCP_THREAD_KILL, which closes the Windows thread handle but leaves it stored. Subsequent disconnect joins/closes the same handle. Actual refused TCP connect during owned presentation renegotiation raised0xc0000008 (loopback-eacf-bfcp-granted/sip-off-0.log2597).

On Windows, release the connect mutex and call existing disconnect/owned-thread joining for failed connection cleanup. This signals closure and retains one close owner, including the retransmission thread. No new destructor path or forced success is added. Non-Windows behavior remains unchanged. Marker: Failed connect still owns real thread handles. Root's owned refused-loopback TCP participant probe plus final actual content lifecycle verifies the correction.
