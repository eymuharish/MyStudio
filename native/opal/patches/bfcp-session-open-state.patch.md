# BFCP actual socket lifecycle

Apply after bfcp-offer-session-reuse.patch and bfcp-failed-connect-join.patch.

The base OpalMediaSession::IsOpen checks an attached OpalMediaTransport. BFCP owns separate libbfcp participant/server sockets, so it always returned false and the offer-only reuse guard never fired. Override IsOpen using successful initialization plus actual started/connected participant and role-owned server sockets. Initial, failed-connect, closed and disconnected sessions remain false. TCP/UDP received-SDP reuse also uses this actual state, and the disconnect callback clears initialization before notifying the application. Close stops and joins both owned wrappers, clears the initialization/floor-owner state, and remains safe when called again by the destructor.

No received-SDP address update is skipped. Actual reoffer/presentation, hold/resume, stop/resume and clean shutdown are checked by the owned localhost LoopbackWorkerTests fixture; ContentCapabilitiesTests also checks initial and closed state. This fixes the structurally ineffective reuse guard reported before staging17E3.
