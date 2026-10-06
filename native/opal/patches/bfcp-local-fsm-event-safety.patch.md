BFCP local FSM event safety

Apply after bfcp-grant-fsm-parser.patch. Local RequestFloor/release has no received wire event; the default NULL pointer is dereferenced by stock unsuccessful-action logging. Stopping before grant, repeating a pending action or a failed send can therefore crash. Owned actual FSM regression reproduces 0xc0000005 (evidence/bfcp-fsm-only-red.log).

Pass a zero-initialized local control event. Keep unsuccessful actions unsuccessful; no local event grants media. Guard logging for other NULL callers, and supply the missing name argument to the three-placeholder success log. No FSM transition table or wire decision is changed. Existing opal_bfcp_nested_status CTest covers pending stop, repeated action, failed send/state retention and bounded successful formatting, GREEN in evidence/bfcp-status-fsm-green.log. Marker: Local control has no received wire status.
