# Architecture changes and ownership — 2026-11-25

| Boundary | Before / failure mode | Current implementation |
| --- | --- | --- |
| TSF event adaptation | Notifications, polling and reentrant state changes could be conflated. | Existing observer is retained; callbacks have lifetime/nesting guards and owner-STA checks. Real events, polling, revalidation and product revocation have distinct counters/state. |
| Target lifetime | Rebind/release order could expose an old target or revive closing state. | Revoke publication first, detach members before COM Release, track context/generation, preserve shutdown as terminal. Async commit still performs live target checks in coordinator and actual edit callback. |
| Real test driver | Association removal was assumed to imply immediate focus loss; activation could still be in flight. | Actual root/child windows, event-driven STA pump and explicit foreground start button in user-assisted mode. No sleep-only transitions, fake notifications or exit-77 success. |
| Source identity | Runtime fingerprint could be confused with complete repository identity. | Runtime source digest and complete repository digest are separate fields. Workflow/docs changes invalidate repository-bound receipts even when runtime fingerprint does not change. |
| Evidence parsing | Script-specific permissive conversions and mutable summary claims. | Shared strict JSON, schema/type, safe path, finite number, JUnit and process helpers; consumers independently reopen and revalidate original bytes. |
| Test completeness | Inventory and JUnit could agree after jointly omitting a test. | Reviewed source-controlled registration contract additionally binds every name, command/argument and property, and CMake source digests. No untrusted CTest command is executed by the consumer. |
| Release checker | Large combined implementation mixed unrelated responsibilities. | `release_verifiers/`: source/build, toolchain/inputs, package, corresponding source, candidate policy/binary, host/CI, installer, stable inputs and final policy aggregation. Existing release standards retained. |
| Qualification harness | A different build's evaluator could be self-hashed and accepted. | Evaluator is built and tested in product tree and uniquely bound to the product receipt; producer and consumer check same current executable and Rime bytes. |
| Installer | MSI/filename and weak helper provenance insufficient. | Real EXE + embedded MSI; independent PE/payload/MSI extraction, version, helper source/build/test receipt, pinned WiX and byte-bound source/toolchain records. |

The frozen chain is clean Git SHA → source/dependency provenance → pinned inputs
and tools → actual build commands → source registration + raw JUnit → product and
test binary hashes → package → Corresponding Source → measured quality/stress/host
evidence → actual owner records → independent release checker. Each arrow is a
consumer check, not a trusted status string. Local hashes establish byte
consistency; trusted retrieval, runner administration and real owner authority
are still required. No local JSON signature field is an authorization.

COM documents, contexts, advice cookies and focus windows are created, called and
released on one live STA. Subscription cleanup is symmetric; callbacks hold a
temporary sink reference. Late callbacks cannot republish shutdown generations.
The observer owner must not destroy it inside an active call/listener callback;
Shutdown is supported there. Unknown focus and revoked permissions fail closed.
Cross-thread destruction is treated as fatal misuse, not safe COM marshaling.

French cache data lasts only for the current admitted session/composition;
completion output/ranking is rebuilt with fresh tokens. A cross-session history,
lowered native work budget and a wholesale replacement of working TSF layers were
rejected because they alter privacy or established behavior without evidence.

Windows/Android portability stops at Unicode scalar/UTF-16 selection contracts,
candidate identity/ranking, session generation/revocation and offline resources.
Windows COM/HWND types must remain inside Windows adapters. Android remains an
unimplemented deliverable, not a Windows acceptance result.
