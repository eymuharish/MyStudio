BFCP failed request list ownership

Owned actual floor request caused heap corruption0xc0000374 after send failure. The lower overload freed its floor list before sendBFCPmessage; wrapper then freed the same pointer whenever send returned-1. The wrapper now owns and frees its temporary list exactly once after the borrowed-list overload returns, including build/validation/send failures and success.

Regression: instantiate a private BFCP participant with floor2, request floor2 without a transport, require failure and normal process exit under owned debugger. Actual SIP content fixture also exercises pending/failed negotiation without a fatal exception.
