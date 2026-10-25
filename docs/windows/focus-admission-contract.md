# Focus admission and target revalidation (P3.13)

This document defines the guard that answers one question before ordinary input or an
ordinary text edit:

> Is the context this coordinator is about to type into still the current TSF input target
> of this thread?

It is not a synonym for `GetFocus() != nullptr`. The guard uses five separate facts, keeps
their evidence sources visible, and refuses conservatively when any of them cannot be
established.

## 1. Evidence chain

Every ordinary admission (new key, candidate acceptance, edit plan) checks, in this order:

1. **Thread input focus.**
   - `ITfThreadFocusSink` is advised on the thread manager with
     `ITfSource::AdviseSink(IID_ITfThreadFocusSink, ...)`. The interface is declared in
     `%WindowsSdk%\Include\10.0.26100.0\um\msctf.h:10398-10417`; Microsoft documents the
     advice call on the `ITfThreadFocusSink` page. `OnSetThreadFocus` and
     `OnKillThreadFocus` update the recorded state. The sink is unadvised with the same
     cookie in `Shutdown`, and the advice/unadvice counts are published.
   - `ITfThreadMgr::IsThreadFocus` (`msctf.h:1101`) is queried live. It is described as
     "TSF input focus", not as global foreground state.
   - When the TSF query cannot confirm focus (this lab process does not register a TIP and
     `IsThreadFocus` returns `S_FALSE`), a **real Win32 thread focus window** is accepted
     only as an explicitly labelled revalidation: `GetGUIThreadInfo(GetCurrentThreadId())`
     reports `hwndFocus`, and its owning thread is the calling thread. The source label is
     `win32-thread-focus-revalidation`; it is never reported as a TSF notification.
   - `GetForegroundWindow` is **not** used for admission. It is displayed in the lab as a
     separate observation only.

2. **The TSF current document manager.** `ITfThreadMgr::GetFocus` (`msctf.h:1090`).

3. **The canonical top context.** `ITfDocumentMgr::GetTop` (`msctf.h:2081`), compared with
   the bound context by canonical `IUnknown` (`QueryInterface(IID_IUnknown)`), not by the
   `ITfContext` interface address.

4. **The identity generation.** `ContextRegistry` must still report the same
   `context_id`/`generation` that the binding was created with. A newer `BeginBinding`
   generation refuses the old binding even if the TSF focus bytes are unchanged.

5. **Real input permission.** The coordinator resolves this separately from the real
   context metadata (`GetStatus`, `KEYBOARD_DISABLED`/`EMPTYCONTEXT` compartments and the
   InputScope application property) before dispatch, again before an edit, and once more in
   the P3.7 callback with the granted cookie.

The pure decision table is `EvaluateTargetEvidence` in
`platforms/windows/tsf_context/target_revalidation.cc`. It is unit-tested without a TSF
manager; the live queries are in `ContextObserver::Revalidate`.

## 2. Evidence sources are not renamed

`TargetRevalidation` carries both a status and a `ThreadFocusEvidence` value:

| Evidence | Meaning |
| --- | --- |
| `thread-focus-notification` | the last thread-focus notification was `OnSetThreadFocus` |
| `thread-focus-kill-notification` | the last thread-focus notification was `OnKillThreadFocus` |
| `query-revalidation` | `IsThreadFocus` answered TRUE |
| `win32-thread-focus-revalidation` | the calling thread owns a real Win32 focus window |
| `none` | no evidence |

The target publication path has a separate `TargetBindingEvidence`:
`notification` (`ITfThreadMgrEventSink::OnSetFocus`), `top-context-notification`
(Push/Pop), `thread-focus-notification`, `revalidation` (`Pump` re-read the real
GetFocus/GetTop), or `initial-adoption`. The lab displays these strings directly. A bind
that followed `GetFocus` revalidation is never displayed as an `OnSetFocus` notification.

## 3. Admission statuses

`SubmitKey`/`SubmitCandidate` gate order is:

```
closed -> busy -> not-bound -> retiring -> blocked latch -> live target -> permission ->
local engine-candidate evaluation -> executor admission
```

The target gate returns one of:

| `SubmitStatus` | Meaning |
| --- | --- |
| `TargetChanged` | `GetFocus`/`GetTop` is no longer the bound context, or the registry generation moved on |
| `ThreadFocusLost` | the thread-focus sink reported a kill, or no live focus evidence exists after a kill |
| `TargetUnverified` | no authority answered, or a required query failed |
| `Accepted` | all five facts were current at admission time |

`DescribeSubmitStage` maps these to UI categories (`focus`, `target-context`,
`permission`, `queue`, `engine-not-ready`, `uncertain`, `retiring`, `busy`, `request`) so a
refusal is not shown as a bare `FAIL`.

A refused submit creates no work item, makes no engine call, and is never replayed. A
target move **after** the engine consumed the event is `NeedsResync` with
`RejectCause::kTargetChangedWhileQueued`, because the engine state and the document may have
diverged.

## 4. Edit classification

The P3.7 `CompositionEditSession` performs the target check at two more points:

* `Precheck` (before `RequestEditSession`), and
* inside `DoEditSession`, after TSF granted the lock and before the permission recheck or
  any writer call.

The callback check calls the same live authority through a `TargetWriteGuard`. The guard
holds a weak reference to the observer state, so a callback that runs after shutdown gets
`false` (refuse) rather than a dangling pointer. A refusal is
`EditStatus::kRejected` / `EditRejectReason::kTargetChanged`; the coordinator maps it to
`NeedsResync` when the engine already ran.

Ordinary plans are `Begin`, `Update`, `Commit`, `Cancel` and candidate acceptance. They must
match the current target at all three points.

`EndOwnedComposition` is different. It is allowed against an old, unfocused context that
this writer really owns, so it is **exempt from the target guard**. It keeps every P3.12
check: ownership/active composition, exact range, real permission, read-only state, and the
Uncertain latch. It never calls `SetText`, never accepts a candidate and never adds a
space. A cleanup that cannot be performed is reported as `CleanupState::kBlocked` with a
named reason; the retirement record may then be dropped from the bounded ledger, but
`finished` is not incremented and the real composition enumeration still shows the live
composition.

## 5. Rebind after a rapid switch or a full ledger

`ObservedBindingController::OnTargetBound` checks admission before creating an engine
client. When the coordinator answers `Retiring` (bounded ledger full) or `Busy`:

* the target is **not** promised bound;
* no engine client is created for it;
* the controller records `rebind_pending` and the refused id/generation for diagnostics.

After `ContextCoordinator::Pump` makes the ledger terminal, the host calls
`ObservedBindingController::Pump`. It does not replay the refused target. It asks the
observer to re-read the **current** `GetFocus`/`GetTop`, and only that fresh publication can
create a client and bind. The observer increments the registry generation for the new
attempt. A refused key, candidate or commit is never stored for replay.

The lab calls the controller's Pump in `LabSession::PumpOnce` after the coordinator's Pump.

## 6. Lab display

The status panel shows these as separate facts:

* actual Win32 focus (child A/B/none), whether the focus HWND belongs to this thread, whether
  a thread focus HWND exists, and whether the foreground window belongs to this process;
* TSF `GetFocus`, `SetFocus` call count/HRESULT and target;
* thread-focus sink advice state, set/kill notification counts, `IsThreadFocus` value,
  query status/HRESULT and evidence source;
* the last bind evidence (notification vs revalidation);
* per-document TSF context id/generation, real text length/selection (lengths only), real
  `ITfCompositionView` enumeration, and mutation counters;
* the bound id/generation, permission, gate, phase, work item, last terminal and
  request/session/operation HRESULTs;
* `Admission` (`Ready`/`Busy`/`Retiring`/`Blocked`/`Unbound`), the last submit status,
  stage, cause, target status, source and query HRESULT;
* target-guard counter values, including the P3.7 precheck/callback/rejection counts;
* live engine clients, executor slots, retirement records, close results and cleanup
  outcomes (including blocked ones);
* the last eight text-free operation lines, including newly refused submits.

No document text, preedit text, field name or app identifier is logged. The document
surfaces paint the fixture text for the experiment, and the status panel reports lengths
and counts only.

## 7. Limits of this environment

* `ITfThreadMgr::IsThreadFocus` returns `S_FALSE` in the non-registered lab process even
  when a real window has Win32 focus. The guard reports that query value and then uses the
  labelled Win32 thread-focus revalidation. A registered TIP in a normal host gets the TSF
  query/sink path.
* The lab observed no `OnSetFocus` or thread-focus notification in this environment; the
  self-test and the focus cases record `notify_* = 0` and a revalidation source. The manual
  check asks the user to click and report whether the counts change; an automated capture is
  not counted as a manual click.
* A newly created empty `ITfDocumentMgr` receives a top context from msctf in this
  environment, and a null `SetFocus` is restored to the associated document while a Win32
  focus window exists. The no-document-manager and no-top branches are therefore pinned by
  the pure decision test and the controlled coordinator status test; the real integration
  case covers a focused manager whose top is not the bound context.
* The Windows offline guarantee remains architectural (no socket calls in our code); this
  lab does not register a TSF service and is not OS-sandboxed.

## 8. Source map

| Concern | File |
| --- | --- |
| pure evidence/decision, write-guard wrapper | `platforms/windows/tsf_context/target_revalidation.{h,cc}` |
| thread-focus sink, live queries, bind evidence | `platforms/windows/tsf_context/context_observer.{h,cc}` |
| submit/edit target gates, statuses, snapshot | `platforms/windows/tsf_coordinator/coordinator.{h,cc}` |
| rebind-after-retirement, write-guard installation | `platforms/windows/tsf_coordinator/observed_binding.{h,cc}` |
| callback-time target check | `platforms/windows/tsf_edit/edit_session.{h,cc}` |
| lab display and self-test | `tools/windows/tsf_lab/` |
| automated cases | `tests/windows/tsf_coordinator/observer_binding_tests.cc`, `coordination_tests.cc`, `tests/windows/tsf_context_observer/observer_tests.cc` |
