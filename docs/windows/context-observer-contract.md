# EpoCanva P3.10 — TSF context observer contract

This document records what `platforms/windows/tsf_context/context_registry`,
`selection_reader`, `context_observer`, and the coordinator-side
`ObservedBindingController` actually are, what was verified against the real TSF manager on
this machine, and what is deliberately not done. It is written after the code and the two
test suites (12 observer cases, 8 observed-coordinator cases), not from a planned design.
The coordinator's own state machine is specified in
[`context-coordinator-contract.md`](context-coordinator-contract.md); P3.10 adds an
observer-driven way to feed it.

## 1. Scope and non-goals

In scope:

* one small STA observer that subscribes to the **real** `ITfThreadMgrEventSink` and to the
  current context's **real** `ITfTextEditSink`;
* the top context of the focused document manager as the binding target, with a
  project-internal identity and generation;
* real selection snapshots: a collapsed insertion point read as an ACP offset in UTF-16
  code units, through a valid read-only cookie;
* own-edit versus external-edit classification with `ITfContext::InWriteSession`;
* immediate invalidation of the old generation on an external text/selection change, and a
  fresh read-only snapshot before a re-bind;
* a bounded, real-context-keyed Uncertain latch that survives replacement of the
  coordinator while the registry lives;
* coordinator wiring: fresh engine client per binding, real caret reconciliation, no
  writes from a notification callback.

Out of scope and **not claimed**:

* no input-method registration, no keyboard sink, no `AdviseKeyEventSink`, no candidate
  window, no language bar, no write into an external application;
* no general event bus or framework: one listener with exactly three methods;
* no reading of document text, clipboard, UIA/OCR or other applications; the selection
  read obtains a range and its ACP extent, never `GetText`;
* no automatic resync, rollback, document clearing, or composition cleanup;
* no OS-level focus guarantee. `ITfThreadMgr::SetFocus` is a real manager call; OnSetFocus
  delivery is environment-dependent and is reported separately (section 9);
* no claim that the Uncertain latch is process-wide or permanent — it is exactly as long-
  lived as the `ContextRegistry` object (section 4).

## 2. Files and responsibilities

```
platforms/windows/tsf_context/
  context_registry.{h,cc}   canonical-IUnknown identity, monotonic ids, generations,
                            bounded capacity, the real-context Uncertain latch
  selection_reader.{h,cc}   collapsed-caret snapshots over ITfRangeACP, with a caller
                            cookie or a real read-only edit session
  context_observer.{h,cc}   ITfThreadMgrEventSink + per-context ITfTextEditSink, top
                            context resolution, own/external classification, Pump
  input_permission.*, permission_query.*   unchanged P3.9

platforms/windows/tsf_coordinator/
  observed_binding.{h,cc}   OnTargetLost/Bound/OwnEdit -> coordinator operations;
                            ObservedEngineClients seam for a fresh client per binding
  coordinator.*             BindObserved, AttachContextRegistry, ObserveRealCaret,
                            registry-keyed gates; all P3.8/P3.9 behaviour retained

tests/windows/tsf_context_observer/   observer verification (real TSF notifications)
tests/windows/tsf_coordinator/
  observer_binding_tests.{h,cc}       coordinator wiring over the observer
```

Division of labour:

| Object | Responsible | Explicitly not responsible |
| --- | --- | --- |
| `ContextRegistry` | canonical identity, id/generation allocation, bounded entries, latch | observing events, reading selections |
| `SelectionReader` | one real read-only session, ACP extent, refusal statuses | binding, invalidation policy |
| `ContextObserver` | real sink subscriptions, top-context resolution, own/external classification, invalidation events, deferred bind | coordinator internals, engine calls, writes |
| `ObservedBindingController` | mapping events to invalidate/bind/caret, fresh client per binding | TSF calls of its own, engine policy |
| `ContextCoordinator` | gates, single flight, edits, terminals; registry-aware latch and real caret | observing TSF events |
| P3.7 `CompositionEditSession` (reused) | real edit sessions and their refusals | observer/binding policy |

## 3. What is subscribed to, and when

Subscribe (`ContextObserver::Subscribe`) does, on the caller's STA:

1. `ITfThreadMgr::AddRef`, `QueryInterface(IID_ITfSource)`,
   `AdviseSink(IID_ITfThreadMgrEventSink, sink, &cookie)`.
2. Reads the **current** `GetFocus`/`GetTop` immediately, so an already-focused document is
   adopted without waiting for a notification.
3. For the current top context: `QueryInterface(IID_ITfSource)` on the context,
   `AdviseSink(IID_ITfTextEditSink, sink, &cookie)`.

On every top-context switch the old context's text sink is unadvised and released, and the
new one is advised. `Shutdown` unadvises both sinks, abandons a pending selection read,
releases every held COM reference and marks the state closed. The COM sink objects hold a
`shared_ptr` to that state, so a late call after shutdown only sees `closed` and touches no
COM pointer. Reference/advice counts are in `ContextObserverSnapshot` and are asserted by
the balance case.

Handled thread-manager notifications:

| Notification | Effect |
| --- | --- |
| `OnSetFocus(pdim, prev)` | adopts `pdim` (NULL means no focused document), re-resolves the top context and drops/binds as needed |
| `OnPushContext(pic)` | only if `pic` belongs to the focused document manager: the pushed context becomes the new top target |
| `OnPopContext(pic)` | only if `pic` belongs to the focused document manager: immediate target drop if it was the current top; `Pump` re-reads `GetTop` |
| `OnUninitDocumentMgr(pdim)` | only if `pdim` is the focused manager: unadvise, release, drop the target, no target remains |
| `OnEndEdit(pic, ecReadOnly, record)` | only for the currently advised context: own/external classification and the caret snapshot |

Background documents are never observed: push/pop events for another document manager are
counted as `background_events_ignored` and change nothing. No text sink is installed on a
background context.

`Pump()` additionally revalidates `GetFocus`. This is a **defensive fallback for missed
focus notifications**, not notification coverage; the real-focus case reports it separately.
It is also what makes a programmatic `SetFocus` observable when msctf does not deliver
`OnSetFocus`. `OnPushContext` and `OnPopContext` are real deliveries in this environment:
they drop the old target synchronously, before any coordinator guard, and back the path-A
integration case.

## 4. Identity, generations, capacity and the latch

`ContextRegistry` identifies a context by its **canonical IUnknown**:
`ITfContext::QueryInterface(IID_IUnknown)`, with one reference held for the entry's
lifetime. Pointer comparison is therefore safe (COM guarantees identity stability; the
address cannot be recycled while our reference exists). No address is written to disk, put
in a value type that outlives a call, or passed to another thread.

* Each new identity gets a monotonic project `context_id` (never reused, never supplied by a
  caller as identity).
* `BeginBinding` increments that context's `generation`. `Observe` is a lookup. A re-focus
  or a rebind produces a strictly newer generation, so an old verdict/plan/callback can be
  rejected by comparison.
* Capacity is bounded (constructor argument, default 64). A new identity at capacity is
  refused with `kCapacityExceeded`; nothing is evicted. An entry is dropped on
  `OnUninitDocumentMgr` only if it is **not** uncertain.
* `MarkUncertain(context)` is keyed by the real context. `IsUncertain(context)` is what the
  coordinator's `BindObserved`, `Bind` and submit gate consult; an integer id supplied by a
  caller has no influence. The latch has no `ClearUncertain`; it ends with the registry
  object. It is not a process-wide or permanent block.

The bounded entries hold COM references. That is deliberate and bounded; the capacity
refusal is the alternative to silently forgetting a latch.

## 5. Selection snapshots

`GetSelection(ec, TF_DEFAULT_SELECTION, 1, &selection, &fetched)`, then the selection range
is QI'd for `ITfRangeACP` and `GetExtent(&anchor_acp, &length_acp)` is read. Both values are
ACP positions in **UTF-16 code units** — the unit TSF's ACP speaks, not bytes and not
graphemes.

Accepted only when all of the following hold:

* the call succeeded and `fetched == 1` with a non-null range;
* `fInterimChar` is FALSE;
* the active end is `TF_AE_START` or `TF_AE_END` (`TF_AE_NONE` is refused rather than
  guessed);
* the range exposes `ITfRangeACP`;
* `length_acp == 0` (collapsed) — any non-zero length, including a negative one, is
  `kNonCollapsed`; and
* `anchor_acp >= 0` — a negative anchor is `kUnsupportedShape`.

There is **no clamp** and no reuse of an older caret. A refused read closes admission: the
observer drops the target and `Pump` publishes nothing until a later notification produces a
reliable collapsed selection.

Two real cookie sources, neither invented:

* `ReadSelectionWithCookie(context, ec)` uses a cookie the caller holds. In
  `OnEndEdit` that is the notification's `ecReadOnly`, which is exactly the read-only
  access the contract permits; no session is started from inside the callback.
* `SelectionReader` requests its own `TF_ES_READ | TF_ES_SYNC` session, falling back to
  `TF_ES_ASYNC` when the document answers `TF_E_SYNCHRONOUS`. A pending read is never used
  as a result. `Abandon()` releases the context immediately and a late callback touches
  only orphaned shared state.

## 6. Own edits versus external edits

`ITfTextEditSink::OnEndEdit` is delivered for a completed **read/write** edit session.
Inside it, `ITfContext::InWriteSession(own_client_id, &in_write)` is the documented way to
learn whether our client performed that session. The observer classifies:

* **own** when `InWriteSession(own_client_id)` is TRUE. The binding is not invalidated and
  the generation does not change. The real caret read with `ecReadOnly` is reported to the
  controller, which reconciles the coordinator's mirror.
* **external** otherwise, when the edit record reports changed text or a changed selection.
  The published target is dropped immediately and the controller calls
  `InvalidateBinding()`; `Pump` later performs a fresh read-only snapshot and re-binds.
* **ignored** when the record reports neither a text nor a selection change (for example a
  property-only session).

A missing/unreadable edit record is treated conservatively: an own edit still updates the
caret, an external edit is treated as a text change and invalidates.

The classification is by the real client id and the completed write session, **not** by
"same thread". The tests use a distinct real `TfClientId`
(`ITfClientId::GetClientId` with a test-only CLSID) for external writes; the own case uses
the host's `Activate` id. Both run on the same STA and the observer distinguishes them.

No text operation is ever issued from `OnEndEdit`. The invalidation path only releases
objects: `CompositionEditSession::Invalidate/Close` and `PermissionSource::Abandon` perform
no TSF call and no write. A plan that would require a text operation is handled by the
coordinator's normal edit session, never inside the notification.

## 7. Ordering: notification, drop, read, bind

Binding is deliberately not performed inside a TSF notification:

```
OnSetFocus / OnPushContext / OnPopContext / OnUninitDocumentMgr / OnEndEdit
        -> drop the published target (release-only invalidation in the controller)
        -> mark a rebind pending
Pump()  (host-called, outside any notification)
        -> revalidate GetFocus
        -> re-read GetTop after a Push/Pop returned
        -> start/advance a real read-only selection session
        -> OnTargetBound (controller: fresh client + BindObserved)
```

Two empirical findings from this machine's `msctf.dll` shaped this:

1. **`OnPopContext` can be delivered before `GetTop` reflects the removal.** A pop is
   therefore used for the immediate drop (the event names the removed context), and `Pump`
   re-reads `GetTop` after the notification has returned. Without that, a pop was silently
   ignored and the binding stayed on the popped context.
2. **A TSF edit session that has not yet reached its store is suspended by a focus change,
   and the thread's session queue can block later writes.** The late-edit case therefore
   fixes the order with a test-only grant hold: TSF is allowed to start the session
   (`RequestLock` reached), the store defers the grant, the grant message is held by the
   host, the focus switch and B's typing complete, and only then is the held grant
   released. The old callback runs genuinely late. On this manager, typing B before A's
   callback is only possible once A's store request has been started; the test proves that
   order explicitly instead of pretending a push alone is enough.

The test host's `SetHoldGrantMessages` control is test-only and disabled in P3.7/P3.8/P3.9
tests.

## 8. Coordinator wiring

`ObservedBindingController` implements the observer's three-method sink:

* `OnTargetLost` calls `ContextCoordinator::InvalidateBinding()` when bound. This
  terminalizes pending work as `AbandonedStale`, abandons a pending permission read and
  destroys the P3.7 session — all release-only.
* `OnTargetBound` requests a **fresh** engine client from the host's
  `ObservedEngineClients` and calls `BindObserved(context, id, generation, caret, client)`.
  The old engine client is never reused: the coordinator discarded the old binding, and a
  new client starts with an empty composition. The controller does not close the old
  client; the executor/host owns it.
* `OnOwnEditCompleted` validates the target id/generation against the coordinator's
  binding and calls `ObserveRealCaret(caret)`. The coordinator applies it immediately when
  idle, or at the work item's terminal, and only for `Applied`/`NoEdit`; the real
  `ecReadOnly` caret wins over the plan-derived mirror.

`AttachContextRegistry` makes the coordinator consult the registry's real-context latch in
`Bind`, `BindObserved` and the submit gate. The controller attaches it in its constructor.
A replacement coordinator/controller that shares the registry cannot bind a latched context
under another integer id; a different context still binds. A direct
`ContextCoordinator::Bind` remains available for controlled unit cases and is documented as
such; the integration path is `BindObserved`.

If `BindObserved` reports `kBusy` (an older engine result is still owed), the controller
calls `Pump()` once and retries the bind. A result that is still executing is left to the
host's drive loop; the observer does not replay the target.

## 9. Real focus: what was actually verified

Verified on this machine with the probe and the real tests:

* `ITfThreadMgr::SetFocus(document_manager)` returns `S_OK` and changes `GetFocus`.
* It did **not** deliver `OnSetFocus` to the thread-manager sink while this process owned no
  OS input focus, even after pumping messages for 400 ms. This is reported as
  **environment-limited**, not as a passing real-focus test.
* `AssociateFocus(hwnd, pdim, &prev)` is the documented convenience that makes TSF call
  `SetFocus` when the associated window receives focus. The SDK documentation states the
  manager does **not** increment `pdimNew`'s reference count; the caller must keep its own
  reference while associated and must release the returned `ppdimPrev`. The test's
  association helper follows that rule. The P3.10 run reported that no visible window could
  be created (`CreateWindowExW` returned NULL with error 0) and assumed an environment
  limit. That assumption was wrong: the test window procedure called
  `DefWindowProcW(window, message, 0, 0)`, discarding `wParam`/`lParam`. `DefWindowProcW`
  needs the `CREATESTRUCT` passed in `lParam` for `WM_NCCREATE`, so the window procedure
  effectively refused creation and `GetLastError` could remain 0. After forwarding both
  parameters unchanged, the visible window is created and `AssociateFocus` succeeds. The
  association path is therefore verified; the OnSetFocus notification remains
  environment-limited.
* The observer's `Pump` revalidation still switches to the new `GetFocus` document. The
  late-edit case uses that real state change and labels it as revalidation; it is not
  counted as an OnSetFocus notification.

The CTest case `windows.tsf_context_observer.real_focus` returns 77 and is registered with
`SKIP_RETURN_CODE 77`: it is **Skipped**, never green, in this environment. It passes only
when a real OnSetFocus is delivered and the observer drops the old target before Pump binds
the new one.

Independent interactive verification:

```
tsf_context_observer_tests.exe --interactive-focus
```

The program creates a real visible window, associates it with a document manager, subscribes
the observer, prints every focus-related observation, and asks the user to click the window.
It reports `OBSERVED` or `NOT-OBSERVED`; it is not an automated CTest case and never claims
an automated pass.

No `SendInput`, no forced system foreground, no input into another application.

## 10. Verification

Environment: Windows 10.0.26200 x64, Visual Studio 17 2022, MSVC 19.44, CMake/CTest 3.31.6,
static CRT, x64 Release for the Windows suites; the pinned English `rime.dll` for the real
cases.

### 10.1 Commands and results

```text
cmake -S platforms/windows/engine_bridge -B build/windows-engine-bridge/test
cmake --build build/windows-engine-bridge/test --config Release
ctest --test-dir build/windows-engine-bridge/test -C Release --no-tests=error
```

| Suite | Command / filter | Result (exit code observed) |
| --- | --- | --- |
| observer suite (P3.10) | `-R "windows.tsf_context_observer"` | 11 passed, **1 skipped** (`real_focus`), exit 0 |
| observed coordinator (P3.10) | `-R "windows.tsf_coordinator.observed"` | 8/8, exit 0 |
| whole engine-bridge tree (P3.4–P3.10) | full CTest | 97 tests, 96 passed, 1 skipped, exit 0 |
| P3.7 edit layer | `build/p37-tsf-edit` | 29/29, exit 0 |
| P3.1 TSF lifecycle | `build/windows-tsf/test` | 3/3, exit 0 |
| P3.2 text | `build/windows-text/test` | 4/4, exit 0 |
| P3.3 input | `build/windows-input/test` | 5/5, exit 0 |
| formal English entry (`tools/test_english_integration.ps1`, tag `p310-r1`) | `build/p39-formal-english` | 29/29, exit 0 |
| root project (Debug) | `build/vs2022-x64-debug` | 7/8, exit 8 — `english.core.spelling_cost_trial_on_pilot_lexicon` measured **67,984.6 µs** worst single call against the unchanged 25,000 µs Debug budget. Reported as a failure; no attribution claimed and no English-core source changed this round. |

Build diagnostics for the engine-bridge build: zero `warning C` / `error C` lines.

### 10.2 Observer evidence (text-free)

```text
evidence initial-binding: one bind event, gen=1, caret=2, one synchronous collapsed read
evidence push-pop: OnPushContext dropped A; Pump bound the child (gen=1, caret=0);
                   OnPopContext dropped the child; Pump bound A again (gen=2, caret=2)
evidence external-selection: loss=ExternalSelectionEdit; old generation invalidated
                   immediately; Pump bound gen=2 caret=5; engine client #2 (fresh)
evidence external-text: loss=ExternalTextEdit; document grew by 1 UTF-16 unit;
                   Pump bound gen=2 caret=3; the next key landed at offset 3
evidence noncollapsed: refusal=SelectionNonCollapsed; no bind; no clamp; text unchanged;
                   a later real collapse re-opened admission at caret=4
evidence own-edit: InWriteSession(own)=1 -> own=1 external=0; no loss, no rebind,
                   gen unchanged; ecReadOnly caret=3; the other client's write is external
evidence document-uninit: OnUninitDocumentMgr delivered; no target; GetFocus has no document
evidence shutdown-late-read: real read pending; observer Shutdown; the late real callback ran
                   on orphaned state (callbacks_after_abandon>=1); no event published;
                   thread/DM/context references balanced
evidence resource-balance: 3 observer lifetimes; every advise paired with an unadvise;
                   all COM references returned; stores=0, advised sinks=0, forced releases=0
```

### 10.3 Coordinator evidence

```text
evidence observed-bind: gen=1 from the observer, caret=2 from the real selection;
                   h applied at offset 2; real OnEndEdit caret=3 won over the plan mirror;
                   one engine client, no loss, no rebind
evidence observed-real-compose: h -> he -> hel with the real English engine; one binding,
                   one generation, one client; every letter was own
evidence observed-external-selection: invalidated immediately; re-bound gen=2 caret=5;
                   fresh client; the next key landed at offset 5
evidence observed-external-text: invalidated; re-bound gen=2 caret=3; fresh client;
                   the next key landed at offset 3
evidence late-permission: A in PermissionPending; real switch while the read was queued;
                   A=AbandonedStale; A's read abandoned; B bound with Unknown;
                   A's late callback: engine_calls=0, mutations=0, completed_queries=0;
                   B typed; exactly one engine call (B's)
evidence late-edit: A EditPending; store request reached the manager and its grant was held
                   (A req=4, pending=1, write grants=0); focus switched to B; B applied
                   (write grant=1); released the held grant late -> A mutations=0,
                   B text unchanged, A's binding not restored
evidence uncertain-swap: real unconfirmed write -> Uncertain; registry latched;
                   coordinator/controller replaced; same context + BindObserved ->
                   BlockedUncertain; Bind with id 9999 -> BlockedUncertain; another
                   document binds and types; the latch is still held
evidence permission-requery: Allowed before the external edit; the re-bound coordinator
                   starts Unknown; read-only metadata -> PermissionDenied, engine_calls=0,
                   mutations=0
```

### 10.4 Brief-item coverage

| # | Required ordering | Case(s) |
| --- | --- | --- |
| 1 | current top context's real binding | `initial_binding`, `observed_bind_and_type` |
| 2 | Push/Pop changes the top context | `push_pop_top` |
| 3 | focus A→B and no valid target | `real_focus` (environment-limited here), `document_uninit` (real OnUninitDocumentMgr; no target) |
| 4 | external caret move, old position not reused | `external_selection`, `observed_external_selection` |
| 5 | non-collapsed selection closes admission | `noncollapsed_selection` |
| 6 | own h→he→hel does not self-invalidate | `own_edit`, `observed_real_compose` |
| 7 | external edit invalidates the old engine binding | `external_text`, `observed_external_text` |
| 8 | A query pending → switch B → A's callback late | `observed_late_permission` |
| 9 | A edit pending → switch B → old callback late | `observed_late_edit` (held grant) |
| 10 | late callbacks modify neither B nor A's admission | both `observed_late_*` cases |
| 11 | permission still re-queried through P3.9 | `observed_permission_requery` |
| 12 | coordinator replacement cannot bypass the same context's Uncertain | `observed_uncertain_swap` |
| 13 | shutdown, then a late notification does not touch destroyed objects | `shutdown_late_read` |
| 14 | all sinks/Context/DocumentMgr/ThreadMgr references balanced | `resource_balance`, `shutdown_late_read` |

## 11. Unimplemented items and known limits

1. **Real OnSetFocus delivery** is not verified in this environment. The automated case is
   skipped; `--interactive-focus` is the independent check. `AssociateFocus`'s reference
   rule is implemented, and a visible window plus a successful association are now
   verified. The P3.10 note that said the window could not be created was a defect in the
   test window procedure (it dropped the creation parameters), corrected after this
   report; it was not an environment limitation.
2. **`OnSetFocus(NULL)`** (focus lost with no document) was not produced by
   `ITfThreadMgr::SetFocus` or `Deactivate` here. The no-target path is covered by a real
   `OnUninitDocumentMgr`/`GetFocus` loss instead. A missed focus change is caught by Pump's
   `GetFocus` revalidation, which is labelled as such.
3. **A queued TSF edit session is suspended by a focus switch.** TSF offers no cancel API;
   the P3.7 session is invalidated (release-only) but the manager may keep the request
   parked until its lock is granted. The late-edit test fixes the order with a held grant.
   A product host must not assume that switching away from a context with a queued edit
   makes that context's TSF session disappear; it should keep the grant pump alive and let
   the callback refuse.
4. **A possibly stale composition is not cleaned up.** Invalidation never issues a text
   operation; an old preedit may remain in the document as a terminated composition. No
   auto-cancel, no auto-commit, no document clearing.
5. **The old engine client is not closed by the controller.** The executor/host owns it;
   the controller requests a fresh client per binding and never continues the old
   composition. Closing the old client is the host's cleanup decision.
6. **The registry's latch ends with the registry.** It is not process-wide, not persistent,
   and has no clear API. A host that destroys and recreates the registry can lose it; that
   is a host contract, not an OS guarantee.
7. **Capacity is a hard bound.** Reaching it refuses a new context (and a new latch); it
   never evicts an entry. The refused context closes admission rather than being tracked
   silently.
8. **`TF_AE_NONE` selections are refused.** A context that reports the default selection
   with no active end cannot be bound by this layer, even if its collapsed range could be
   interpreted. This is a conservative choice, documented rather than guessed.
9. **`InWriteSession` reflects the TIP's client id, not the author of the text.** A write
   performed by another component sharing our client id is classified as our own; the
   distinction is by client identity and completed write session, which is the documented
   TSF mechanism. The tests use a distinct real client id for external writes.
10. **The existing English-core Debug performance failure** (67,984.6 µs vs 25,000 µs) is
    not fixed, not re-baselined and not attributed to this round.
