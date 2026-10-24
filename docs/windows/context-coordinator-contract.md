# EpoCanva P3.8 — context coordinator contract

This document records what `platforms/windows/tsf_coordinator/` actually is, what was
verified, and what it deliberately does not do. It is written after the code and the
46-case suite (14 coordination cases + 18 real-engine cases + falsifiability controls),
not from a planned design.

## 1. Scope and non-goals

In scope:

* one coordinator object per STA thread, binding **one** TSF context to **one** engine
  client at a time;
* at most **one** in-flight work item (a single key dispatch or a single candidate
  acceptance); a second submit is `Busy`, never queued;
* controlled input submitted explicitly by the host; a pure local `Evaluate` that
  submits nothing;
* an explicit permission gate (Allowed/Denied/Unknown), host-supplied;
* explicit binding invalidation with a generation bump, under which pending work is
  terminalized and old results are discarded without reaching any document;
* real engine results landing in the real P3.7 text store through real edit sessions.

Out of scope and **not claimed**:

* no input-method registration, no keyboard sink, no `AdviseKeyEventSink`, no candidate
  window, no language bar, no write into any external application;
* no multi-request pipeline, no general message bus, no application framework;
* no real password/one-time-code field detection — the permission is an explicit host
  gate, and no document is ever read to classify a field;
* no connection to the real TSF service activation path or the P3.6 broker — the test
  process holds the executor explicitly;
* no `pfEaten` decision of any kind: accepting a work item promises nothing about key
  consumption.

## 2. Files and responsibilities

```
platforms/windows/tsf_coordinator/
  CMakeLists.txt         added by platforms/windows/engine_bridge (existing targets reused)
  coordinator.h/.cc      the coordinator: binding, gates, single flight, terminals, close
  engine_to_plan.h/.cc   the pure engine-observation -> edit-decision mapping

tests/windows/tsf_coordinator/
  coordinator_test_support.{h,cc}  bounded drive loop, harnesses, text-free checks
  coordination_tests.{h,cc}        stage 1: controlled engine seam, real TSF store
  real_engine_tests.{h,cc}         stage 2: the real English engine end to end
```

Division of labour:

| Object | Responsible | Explicitly not responsible |
| --- | --- | --- |
| `ContextCoordinator` | binding record, gate order, single flight, work terminals, lockdowns, close | engine decisions, TSF call details, candidate UI |
| `DecidePlanFromEngine` | the pure observation→decision mapping incl. the refused shapes | building plans (needs identity), any I/O |
| `EngineExecutor` (P3.5, reused) | worker-thread engine calls, value results, capacity | anything TSF |
| `CompositionEditSession` (P3.7, reused) | edit sessions, identity/generation checks, permission/refusal semantics, uncertain latch | engine calls |
| P3.7 `TestHost`/`TestTextStore` (reused) | the real TSF document with fault knobs | a second host is written nowhere |

The coordinator holds an `EngineExecutor&` it never starts, stops or joins; the engine
client ids are supplied by the host at bind time. No `RimeRuntime`/`RimeSession`/
`InputController` pointer exists anywhere in this layer.

## 3. The binding record and generations

A binding records: the `ITfContext` (held by the P3.7 session), a `ContextIdentity`
{context_id (host-assigned), generation (coordinator-assigned, incremented on every
bind)}, the engine `ClientId`, the explicit `InputPermission`, the current work item, and
the coordinator's document mirror (tracked caret, composition start/length/text — UTF-16
code units throughout). Every bind validates its arguments before its state; the argument
rules are: non-null context, non-zero context id, valid engine client, non-negative caret.

* Every new binding starts at permission **Unknown** (the safe refusal); the host grants
  `Allowed` explicitly.
* `InvalidateBinding` unbinds, invalidates and closes the P3.7 session, and resets the
  mirror. A plan built for an old generation is refused by the P3.7 callback (`Stale`)
  and by the coordinator alike; an old generation's engine result is consumed once and
  discarded (section 7).
* Re-binding while a previous work item's engine result is still owed returns `Busy`
  until the discard completes: single flight covers the whole work lifetime, not just the
  accepted phase.

## 4. One work item: phases and terminals

```
        SubmitKey/SubmitCandidate (gates pass)
 Idle ------------------------------> EnginePending
                                          | Pump: TakeResult (exactly once)
                                          v
                                    pure conversion --NoEdit--> Idle(NoEdit)
                                          | plan
                                          v
                                      EditPending --callback completed--> terminal
```

Terminal outcomes and what each one claims:

| Outcome | Claim |
| --- | --- |
| `Applied` | the edit session callback ran and every step, including the text write, succeeded |
| `NoEdit` | the engine consumed the event and the document already matches (or nothing changed) |
| `Rejected` | a gate refused the work, or the edit was cleanly refused (see `cause` / last-edit fields) |
| `NeedsResync` | the engine was dispatched but its observation is unusable; no retry, no replay, blocked until explicit invalidation |
| `Uncertain` | a TSF write could not be confirmed (P3.7 `FailedPossiblyMutated`/`FailedAfterMutation`); this context is locked down |
| `UnsupportedShape` | the engine reported a commit and a live preedit in one observation; refused whole, this binding is blocked |
| `AbandonedStale` | invalidated or closed while pending; the old result is never applied to any context |

`TF_S_ASYNC` stays `EditPending` — waiting is not applying. An async edit whose callback
never runs (observed with the host's `refuse_write_locks` knob: msctf queues the request,
the store never grants the lock, `DoEditSession` never happens) never reaches a terminal
edit result; the work still gets its terminal via the **explicit invalidation or close**,
which is deliberately not written inside `DoEditSession`. The tests bound their waits; the
coordinator has no clock and no timeout policy of its own.

## 5. Gates and their order

`SubmitKey`/`SubmitCandidate` check, in this order (the order is part of the contract):

1. closed;
2. a work item is unresolved → **Busy** (single flight outranks everything below);
3. not bound;
4. a block latch: uncertain (same context), needs-resync, unsupported shape;
5. permission: **Denied**, then **Unknown** — neither dispatches to the engine nor
   requests an edit;
6. the local pure evaluation (`P3.3 DecideKey` over the coordinator's own context facts)
   — a non-engine-candidate key is refused before the executor is touched;
7. executor admission (`NotRunning`/`QueueFull`/`ClientUnavailable`/… mapped verbatim).

`Evaluate` is pure: it calls `DecideKey` on local facts only, submits nothing, and
repeating it changes nothing (asserted against the executor capacity and the store
counters in both suites).

## 6. EngineUpdate → edit decision (the pure mapping)

Input: the coordinator's `LocalDocumentView` (composition active/start/text, tracked
caret) and one `EngineObservation` (copied `EngineState` + `EngineCommit` + the coarse
dispatch facts). Rules, in order:

| # | Condition | Decision |
| --- | --- | --- |
| 1 | engine refused or no engine call was made | `NoEdit` |
| 2 | state unreadable, or commit status not `kNoCommit`/`kRead` | `InvalidObservation` → NeedsResync |
| 3 | commit present **and** engine still composing with preedit | `UnsupportedShape` (refused whole — never half-applied) |
| 4a | commit present, local composition active | `Commit(commit text)` (replaces the composition — no duplicate insertion) |
| 4b | commit present, no composition, empty commit text | `NoEdit` (nothing insertable) |
| 4c | commit present, no composition | `Commit` at the tracked caret (insert once via P3.7's no-preedit path) |
| 5a | no commit, engine preedit, no local composition | `Begin(preedit)` at the tracked caret |
| 5b | no commit, engine preedit equals the displayed text | `NoEdit` (already shown) |
| 5c | no commit, engine preedit differs | `Update(preedit)` (replaces the composition range) |
| 6a | no commit, engine not composing, local composition active | `Cancel` (removes only the writer's own text) |
| 6b | no commit, nothing composing anywhere | `NoEdit` |

The committed/committed-with-space shape ("hel ", "hello ") keeps the engine's commit
string verbatim, so P2.5's trailing space arrives as part of one commit. The mapping sees
no COM pointer and reads no document; its diagnostics are text-free. The plan itself is
built by the coordinator with the binding's identity; engine text crosses UTF-16→UTF-8
through the strict P3.2 converter (a malformed value is a NeedsResync, never a repair).

## 7. Result consumption: once, and copied

The engine result is taken exactly once (`TakeResult`); the executor slot is released at
that moment, which is **not** an application claim. The conversion runs on the
coordinator's own copy. When a work item was invalidated before its result arrived, the
result is still consumed once (slot hygiene) and discarded without conversion — the
document(s) receive nothing, asserted in both suites.

## 8. Invalidation while pending

* **Engine pending**: the work is terminalized `AbandonedStale` immediately; the phase
  stays engine-pending until the single discard consumes the result. `Bind` returns
  `Busy` meanwhile. (Deterministically exercised with the controlled backend's dispatch
  gate; with the real engine the result is ready-but-unconsumed at invalidation time —
  the same single-consumption invariant is what the case asserts.)
* **Edit pending**: the work is terminalized `AbandonedStale` immediately, the phase goes
  idle (a callback that never runs cannot keep the coordinator pending), and the session
  is invalidated, closed and destroyed. If the deferred callback runs afterwards it
  refuses inside P3.7 (closed/not-bound checks) and writes nothing; if the edit had
  already applied **before** the invalidation, the resolved outcome says `Applied`
  honestly instead of denying a real write.

## 9. Permission

`Unknown` and `Denied` never dispatch and never request an edit, at submit time and again
at conversion time. A permission change while the engine result is queued rejects that
work item (`PermissionChangedWhileQueued`) even though the engine already consumed the
key — the engine consumption is counted and printed as evidence, the document is not
touched. A change while an edit is pending is caught again **inside the real callback**
(P3.7's own permission re-check; the store observes a granted lock, the callback refuses,
no text call happens). The permission is forwarded to the session, so both layers restate
the same gate.

## 10. The uncertain lockdown

When P3.7 reports `FailedPossiblyMutated` or `FailedAfterMutation`, the coordinator
records `Uncertain` and blocks **that context id** (injected in the tests through the
P3.7 host's `MutationFault` knob while the engine is real; the document's exact content
is deliberately not asserted — the coordinator does not know it). Further submits are
refused (`BlockedUncertain`); `InvalidateBinding` is allowed, but re-binding the **same**
context id is refused — the lockdown cannot be bypassed by re-binding the writer. A
different context binds and works normally. Only `Close()` clears the block. Cleanup of
the possibly-mutated text is deliberately not attempted: no auto-commit, no auto-cancel
of unknown text (P3.7's session is the terminal for editing there, and this layer adds no
second one).

`NeedsResync` (engine observation unusable — engine command faulted or produced an
unmappable observation) and `UnsupportedShape` (commit + preedit in one observation)
block normal input the same way; their block is cleared by an explicit invalidation
because the document text is known and only the engine view is not. No key is ever
auto-retried or replayed in any of these states.

## 11. Pump and driving

`Pump()` never waits: it takes a ready engine result, converts it, submits the edit, and
harvests an edit result whose completion event is already signalled. It never calls
`WaitForResult`, never waits on a TSF lock and never dispatches STA messages. The host
owns bounded waiting: `DriveToCompletion` (test code) waits on the two real completion
facts — the executor result while engine-pending (no TSF work is outstanding then, so the
STA is free to block briefly), and the P3.7 completion event while edit-pending, with the
host's message loop dispatching the deferred grants. Completion is judged from the
coordinator's state alone; no sleep is consulted. This is an explicit-drive test
mechanism, not a product notification design — no UI-thread notification mechanism is
claimed.

## 12. Close

`Close()` is terminal and idempotent. Pending work becomes `AbandonedStale`; a ready
engine result is taken-and-discarded, an unready one is left to the executor's own stop
drain (the tests surface the leftover slot honestly and take it once). The session is
invalidated, closed and destroyed — release-only, no text operation, no assumption that
`DoEditSession` will ever run. Submit/Bind after close return `Closed`.

## 13. Build wiring

The coordinator is added by `platforms/windows/engine_bridge/CMakeLists.txt` and reuses
its existing targets: `epocanva_engine_executor` (and through it the P3.2/P3.3/P3.4
libraries) are **linked**, not recompiled. The P3.7 edit-layer sources are compiled once
per build tree from their one set of files into `epocanva_tsf_edit_layer`; the strict
P3.2 converter symbols are not recompiled — they come from `epocanva_engine_text`. The
tests compile the P3.7 host and the P3.5 scaffolding (fixtures, deployment, real
`ToUnicodeEx` key helpers, controlled backend) from their one set of sources. No new
top-level project, no new text/key symbols, no registry write, static CRT (/MT) as the
whole tree.

## 14. Verification

Environment: Windows 10.0.26200 x64, Visual Studio 17 2022, MSVC 19.44, CMake/CTest
3.31.6, static CRT, x64 Release; the pinned English `rime.dll`
(`build/librime-english/rime/install/lib/rime.dll`) and the fixed fixtures.

```text
cmake -S platforms/windows/engine_bridge -B build/windows-engine-bridge/test
cmake --build build/windows-engine-bridge/test --config Release
ctest --test-dir build/windows-engine-bridge/test -C Release --no-tests=error
```

Results (exit codes observed):

| Suite | Filter / directory | Result |
| --- | --- | --- |
| P3.8 stage 1 (controlled engine seam, real TSF) | `windows\.tsf_coordinator\.` minus `real_` | 14/14 |
| P3.8 stage 2 (real English engine, real TSF) | `windows\.tsf_coordinator\.real_` | 18/18 |
| whole engine-bridge tree (P3.4–P3.6 + P3.8) | — | 54/54 |
| P3.7 edit layer | `build/p37-tsf-edit` | 29/29 |
| P3.1 TSF lifecycle | `build/windows-tsf/test` | 3/3 |
| P3.2 text | `build/windows-text/test` | 4/4 |
| P3.3 input | `build/windows-input/test` | 5/5 |
| formal English entry (`tools/test_english_integration.ps1`, tag `p38-regress`) | `build/p37-formal-english` | 29/29 |
| root project (Debug) | `build/vs2022-x64-debug` | 7/8 — the known pre-existing `english.core.spelling_cost_trial_on_pilot_lexicon` Debug budget failure; this round changed no English-core source and recompiled none of that binary. Reported as a failure, not smoothed over. |

### 14.1 Evidence lines (text-free; from the delivered logs)

```text
evidence compose-progress: letter: outcome=Applied mutations=1 doc_len=1   (then 2/2, 3/3)
evidence candidate: Space after selection -> document holds "hello " exactly once
evidence async-generation: the old callback changed nothing in the old document (mutations=0, starts=0)
evidence real-refusal read-only: reason=ReadOnlyContext mutations=0
evidence real-refusal write-lock-refused: invalidation terminalized the stuck edit (AbandonedStale)
evidence real-refusal disconnected: reason=ContextDisconnected mutations=0
evidence uncertain: the unconfirmed write latched Uncertain; same-context re-bind refused
evidence destroyed-context: outcome=AbandonedStale phase=Idle via=close
```

### 14.2 Brief-item coverage

| # | Brief requirement (section 9) | Case(s) |
| --- | --- | --- |
| 1 | h→he→hel stepwise | `real_compose_progress`, `composition_progress_controlled` |
| 2 | Return leaves one "hel" | `real_return_commit` |
| 3 | Space leaves "hel " | `real_space_commit` |
| 4 | accepted "hello" + Space, no duplicate | `real_candidate_commit` |
| 5 | BackSpace updates, Escape cancels own | `real_backspace_escape` |
| 6 | "hello world" with prefix/suffix intact | `real_surrounding_text` |
| 7 | technical text verbatim | `real_technical_text` |
| 8 | Unicode prefix/suffix untouched | `real_unicode_affixes` (incl. an astral suffix) |
| 9 | repeated Evaluate changes nothing | `real_evaluate_pure`, `evaluate_pure` |
| 10 | Busy rejects, never executes twice | `real_busy_reject`, `busy_single_flight` |
| 11 | rebind while engine result pending, old result writes nothing | `real_rebind_pending`, `engine_pending_invalidate` |
| 12 | async edit + generation change, old callback writes neither doc | `real_async_generation`, `edit_pending_invalidate` |
| 13 | Allowed→Denied/Unknown: no further edits | `real_permission_flip` (both re-verification points), `permission_change_queued`, `permission_change_edit_pending` |
| 14 | read-only / refused locks / disconnect never Applied | `real_refusals_not_applied`, `refusals_not_applied` |
| 15 | a possibly-mutated failure blocks, later input not auto-replayed | `real_uncertain_lockdown` (TSF-side, real engine + host fault knob) and `needs_resync_block` (engine-side fault through the existing controlled seam; not inducible through the real engine, stated as such) |
| 16 | repeated Pump/reads insert nothing twice | `real_pump_idempotent`, `pump_idempotent`, `busy_single_flight` |
| 17 | Close with pending engine/edit work: resources cleaned, states explicit | `real_close_pending`, `close_pending_work` |
| 18 | destroyed context's unexecuted callback path waits forever nowhere | `real_destroyed_context` |

Plus the pure mapping matrix `conversion_pure` (including the refused `UnsupportedShape`
and `InvalidObservation` shapes) and the falsifiability control `can_fail`.

## 15. Unimplemented items and known limits

1. No sink, no registration, no candidate UI, no broker wiring: the P3.6 broker and any
   real activation path are later phases; the test process holds the executor.
2. No `pfEaten` policy and no key-eating claim of any kind.
3. Commit + live preedit in one observation is refused (`UnsupportedShape`), not
   sequenced; the pinned English engine does not produce it today (the guard exists so a
   future engine change fails loudly).
4. An engine-side `NeedsResync` cannot be induced through the real engine; that path is
   covered through the existing controlled backend seam (declared test seam), while every
   normal-path case uses the real engine.
5. An async edit whose lock is never granted (msctf queued it; the store refuses) never
   produces an edit result; recovery is the explicit invalidation/close, and the tests
   bound their waits. No coordinator-side timeout policy exists by design.
6. Mid-composition invalidation leaves the old document's preedit text in place as a
   terminated composition; automatic cleanup of a known-text composition on invalidation
   is deliberately not implemented (the brief's "necessary cleanup takes an independent
   path" is future work). Nothing commits or cancels unknown text.
7. One engine client's composition state survives a coordinator rebinding (the engine
   session is the host's); the tests bind a fresh client for a fresh document, and
   same-client continuation across bindings is not modelled.
8. The caret mirror starts from the host-asserted bind-time caret and is maintained only
   from applied edits; external caret movement is not tracked (a `SelectionMoved` refusal
   surfaces it honestly as a rejected edit).
9. The existing English-core Debug performance budget failure (14) is not fixed, not
   re-baselined and not attributed to this round.
