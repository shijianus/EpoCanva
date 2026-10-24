# EpoCanva P3.7 — controlled TSF edit contract

This document records what the P3.7 composition/edit layer actually does, what it was verified
against, and what it deliberately does not do. It is written after the code and the 24-case
suite, not from a planned design.

## 1. Scope and non-goals

In scope:

* a controlled, real TSF test host (`ITextStoreACP` + real `ITfThreadMgr`, `ITfDocumentMgr`,
  `ITfContext`, `ITfEditSession`, document locks);
* an immutable, COM-free `EditPlan` and a `CompositionEditSession` that applies it through
  `ITfContext::RequestEditSession`;
* Begin/Update/Commit/Cancel preedit operations with UTF-16 code-unit offsets;
* real composed-range edits through `ITfRange::SetText`, `ITfComposition::EndComposition` and
  `ITfInsertAtSelection::InsertTextAtSelection`;
* identity/generation checks, input-permission refusal, read-only refusal, asynchronous lock
  handling, stale-plan refusal, and explicit failure statuses;
* 18 behavioural composition cases plus a composition falsifiability control, alongside
  four host cases and a host falsifiability control (24 CTest cases in total).

Out of scope and **not claimed**:

* no input-method registration, no keyboard sink (`AdviseKeyEventSink`), no key handling in
  an external application, no candidate window, no language-bar entry;
* no real password-field or one-time-code-field identification;
* no grapheme-cluster editing or grapheme-aware caret movement;
* no automatic retry, no rollback of a partially applied edit, no replay of a failed commit;
* no system-wide input-method availability claim. The layer is exercised inside a test
  process against the real TSF manager; it is not installed anywhere.

## 2. Units and text safety

* `EditPlan::text_utf16()` is UTF-16 code units.
* `selection_start_utf16` / `selection_end_utf16` are ACP positions, i.e. UTF-16 code-unit
  offsets in the document. They are not UTF-8 bytes, not caret columns and not grapheme
  clusters.
* Only a collapsed selection (`start == end`) is representable. `EditPlan::BuildBegin` and
  `BuildCommit` refuse a non-collapsed or negative selection with
  `SelectionNotCollapsed` / `InvalidSelection`; the executor also refuses an actual
  non-collapsed selection at the context.
* Plan text is validated by the P3.2 strict converter
  (`platforms/windows/text/text_encoding.*`, `ConvertUtf8BytesToUtf16CodeUnits`): malformed
  UTF-8 is refused with an empty plan and a text-free diagnostic; U+FFFC is refused because
  `ITfRange::SetText` must not receive the embedded-object placeholder.
* A caret inside a surrogate pair is refused before `StartComposition`
  (`SelectionInsideSurrogatePair`). The check clones the selection range and uses
  `ShiftStart(-1)` / `ShiftEnd(+1)` + `GetText`; it does not split a pair, normalise text,
  trim, or clamp an offset.
* No step in the product layer logs text, composition buffers, or focused-field identifiers.
  `EditResult` carries offsets and HRESULTs only.

## 3. The controlled TSF test host

### 3.1 Real objects

`tests/windows/tsf_edit/test_host.cc` creates, on one STA thread:

| Object | How |
| --- | --- |
| `ITfThreadMgr` | `CoCreateInstance(CLSID_TF_ThreadMgr)` after `CoInitializeEx(COINIT_APARTMENTTHREADED)` |
| client id | `ITfThreadMgr::Activate` |
| `ITfDocumentMgr` | `ITfThreadMgr::CreateDocumentMgr` |
| `ITfContext` | `ITfDocumentMgr::CreateContext(client_id, 0, store, &context, &cookie)` |
| document stack | `ITfDocumentMgr::Push(context)` |
| message loop | a message-only window + `MsgWaitForMultipleObjects` |

`ITfDocumentMgr::CreateContext` also queries the object passed as `punk` for
`ITfContextOwnerCompositionSink` and installs it; that is why `TestTextStore` implements that
second interface. Advising it through `ITfSource::AdviseSink` was tried and returned
`TS_E_NOOBJECT` (`0x80040202`); the `CreateContext` query path is the documented one
(MS Learn, `ITfContextOwnerCompositionSink`: "When the application calls
`ITfDocumentMgr::CreateContext`, the TSF manager queries the object for this interface").

`SetFocus` is deliberately not used. In this headless test process it returned `E_FAIL`
(`0x80004005`), and `RequestEditSession` works without it. Focus/`AssociateFocus` behaviour is
therefore **unverified** and is listed in section 9.

### 3.2 The `ITextStoreACP` surface

Implemented and exercised: sink advice, `RequestLock`, `GetStatus`, `QueryInsert`,
`GetSelection`, `SetSelection`, `GetText`, `SetText`, `InsertTextAtSelection`,
`GetEndACP`, `GetActiveView`, the attribute no-op surface, and the context-owner composition
sink.

Unsupported capabilities return the interface's own refusal, not `S_OK`:

| Method | Result | Reason |
| --- | --- | --- |
| `GetTextExt`, `GetScreenExt`, `GetACPFromPoint` | `TS_E_NOLAYOUT` | no layout exists |
| `GetWnd` | `E_NOTIMPL` | the store is not an edit control |
| `GetFormattedText`, `GetEmbedded`, `InsertEmbedded`, `InsertEmbeddedAtSelection` | `E_NOTIMPL` | plain text only |
| `QueryInsertEmbedded` | `S_OK`, `FALSE` | the documented "not insertable" answer |
| `RequestSupportedAttrs`, `RequestAttrs*`, `FindNextAttrTransition`, `RetrieveRequestedAttrs` | `S_OK`, zero attributes | no attribute storage |

Text methods enforce the lock they require (`TS_E_NOLOCK`), read-only (`TS_E_READONLY`),
position validation (`TS_E_INVALIDPOS`), well-formed UTF-16, and the absence of U+FFFC.
`SetText`/`InsertTextAtSelection` never call `OnTextChange` in response to a TSF-initiated
call, per the interface documentation. The fault-injection path returns a failure and does
not notify either; see 3.4 for why TSF's cached view then remains old.

### 3.3 Lock protocol

The store distinguishes `TS_LF_READ` (`0x2`), `TS_LF_READWRITE` (`0x6`) and `TS_LF_SYNC`
(`0x1`) from `textstor.h`. `RequestLock` behaves as follows:

* not locked, synchronous → call `ITextStoreACPSink::OnLockGranted` with the read/write bits
  only (not `TS_LF_SYNC`), return `S_OK`, and set `*phrSession` to the sink's result;
* not locked, asynchronous and `async_locks` enabled → set `*phrSession = TS_S_ASYNC`, return
  `S_OK`, post a message, and grant later from the host's message loop;
* locked, synchronous → set `*phrSession = TS_E_SYNCHRONOUS`, do not grant;
* locked, asynchronous → queue the request; a write request during a read lock is the
  documented upgrade and is granted after the current `OnLockGranted` returns;
* a read-only document or `refuse_write_locks` → `*phrSession = TS_E_READONLY`, no grant.

The lock exists only for the duration of `OnLockGranted`; the store unsets it before
returning. `TF_ES_*`, `TF_S_ASYNC`, `TF_E_*` are used for `ITfContext::RequestEditSession`;
`TS_LF_*`, `TS_S_ASYNC`, `TS_E_*` are used in the store's `ITextStoreACP`/sink methods. Both
success constants have the numeric value `0x00040300`, but the named constant used at each
interface is the one that belongs there.

### 3.4 Host state and fault injection

`TestTextStore` controls the test only:

* `initial_text_utf16` / collapsed `initial_selection_*` (host setup, before any edit);
* `read_only` (reported through `GetStatus` as `TS_SD_READONLY`);
* `refuse_write_locks` (writable status, but every read/write lock request is refused);
* `async_locks` (defer `RequestLock` to the message loop);
* `refuse_compositions` (the owner composition sink sets `*pfOk = FALSE`);
* `MutationFault`: `kFailBeforeMutation` returns `E_FAIL` after validation, with no text
  change; `kFailAfterMutation` applies the change and returns `E_UNEXPECTED`.

`E_UNEXPECTED` is the controlled store's explicit **partial-application signal**. It is not a
TSF guarantee. It exists because TSF keeps its own cached view of the text: when the store
mutates and returns a plain failure, TSF's ranges still read the old text, so the layer could
not distinguish the two cases by read-back. The layer treats `E_UNEXPECTED` from the mutating
call as "may have changed, not verified" (`FailedAfterMutation`, `mutation_verified = false`).
A store that silently mutates and returns a plain `E_FAIL` is outside what this layer can
distinguish; that limitation is recorded in section 9.

### 3.5 Teardown and reference accounting

Shutdown order, all on the STA:

1. drain queued grant messages;
2. `ITfDocumentMgr::Pop(TF_POPF_ALL)` while TSF still knows the context and its composition
   sink. This delivers `OnEndComposition` for an active composition and releases the
   context's references to the text store and to the context-owner composition sink;
3. if TSF somehow left the store sink advised, call the application-side
   `ITextStoreACP::UnadviseSink` as a fallback;
4. release the host's `ITfContext`, `ITfDocumentMgr` and store references;
5. `ITfThreadMgr::Deactivate`, release the ThreadMgr;
6. destroy the message-only window, `CoUninitialize`.

**Correction to the stage-1 host.** Commit
`b208184674d9ed642e05cc875b54d179cfeacc11` (`test(windows): add controlled TSF text-store
host`) never set `TestDocument::connected_` to true after `Push`, so `Pop` was skipped during
teardown. That made stage 1's cleanup case look green while TSF still held references; the
"msctf retains two references" explanation produced during that investigation was an artifact
of the missing Pop and is withdrawn. Commit 2 sets `connected_ = true` after a successful
`Push`; the corrected host-cleanup and resource-cleanup cases now show:

```
teardown: pop=0x00000000 sink_before=1 sink_after_pop=0 refs_before=1
          refs_after_context=1 refs_after_docmgr=1 live_after_host_release=0
          final_refs=0 addref=4 release=4 qi_textstore=1 qi_sink=1
```

i.e. `Pop` releases every TSF reference, the sink is unadvised, the host releases the last
reference, and the store object is destroyed. `AddRef`/`Release` are balanced at 4/4 for the
tested flow; the composition test reports `store_refs=0 live_stores=0 sink_before=1
sink_after_pop=0 addref=4 release=4 composition_ends=1`.

## 4. Edit plans and results

### 4.1 `EditPlan`

`platforms/windows/tsf_edit/edit_plan.h` defines an immutable value type with:

* operation (`BeginPreedit`, `UpdatePreedit`, `Commit`, `Cancel`);
* `ContextIdentity { context_id, generation }` (both non-zero);
* validated UTF-16 text;
* collapsed UTF-16 selection offsets for Begin/Commit; `-1` otherwise;
* a monotonic sequence number.

There is no setter, no COM pointer, no `ITfContext`, no range, and no edit cookie. A plan is
built only through `BuildBegin` / `BuildUpdate` / `BuildCommit` / `BuildCancel`; a failed build
carries no plan.

### 4.2 `EditResult`

`edit_result.h` records, separately:

* `RequestEditSession`'s HRESULT (`request_hr`);
* its `phrSession` (`phr_session_hr`, `TF_S_ASYNC` `0x00040300` for a queued request);
* `DoEditSession`'s HRESULT (`edit_session_hr`);
* the first failing TSF operation HRESULT (`operation_hr`) and `EndComposition`'s HRESULT;
* UTF-16 composition start/length;
* `text_may_have_changed` / `mutation_verified`;
* `composition_active`, `lock_observed`, session id and plan identity/sequence.

Status vocabulary:

| Status | Meaning |
| --- | --- |
| `Pending` | `TF_S_ASYNC`; waiting only, nothing applied |
| `Applied` | the callback ran and the write succeeded |
| `Rejected` | a precondition/policy refused before a TSF write |
| `FailedBeforeMutation` | a TSF call failed and the text was verified unchanged |
| `FailedAfterMutation` | a call failed after the text changed; no rollback claim |
| `Stale` | context identity/generation no longer matches; no write performed |

## 5. Applying a plan

### 5.1 Synchronous event order

For a successful `Begin` in the test host, the observed order is:

1. `ITfContext::RequestEditSession(client_id, callback, TF_ES_SYNC|TF_ES_READWRITE, &phr)`;
2. msctf calls `ITextStoreACP::RequestLock(TS_LF_READWRITE|TS_LF_SYNC, &phrSession)`;
3. the store calls the real `ITextStoreACPSink::OnLockGranted(TS_LF_READWRITE)`;
4. msctf calls `ITfEditSession::DoEditSession(ec)` inside that grant;
5. the callback rechecks closed/bound/identity/generation/permission/read-only, then the
   writer:
   * `ITfContext::GetSelection` → `ITfRangeACP::GetExtent` (collapsed, expected caret);
   * scalar-boundary check via range clones;
   * `ITfContextComposition::StartComposition(ec, range, sink, &composition)`;
   * `ITfComposition::GetRange` and `ITfRange::SetText` for the preedit;
6. `DoEditSession` returns; `OnLockGranted` returns; the lock ends; `RequestEditSession`
   returns `S_OK` with the session HRESULT.

The host's evidence line for this case is:
`request_lock=1 write_locks=1 mutations=1 callbacks=1 locks_observed=1`.

### 5.2 Asynchronous

`ApplyAsync` submits `TF_ES_ASYNC|TF_ES_READWRITE`. If the manager accepts a deferred lock,
`phrSession` is `TF_S_ASYNC` and the result is `Pending`. The host's store returns
`TS_S_ASYNC` from `RequestLock` and posts a message; the text remains untouched until
`MsgWaitForMultipleObjects` + the message loop dispatch the grant. Only then does
`DoEditSession` run. No sleep/poll loop is used; the completion event is signalled by the
callback.

Observed: `pending_status=Pending final_status=Applied request_hr=0x00000000
phr=0x00040300 callbacks=1 async_grants=1`. The final result keeps the original
`TF_S_ASYNC` phrase and the callback's edit HRESULT separately.

If `RequestEditSession` returns an error, `phrSession` is mapped (`TF_E_DISCONNECTED`,
`TF_E_LOCKED`, `TF_E_SYNCHRONOUS`, `TS_E_READONLY`, other) to `Rejected` or
`FailedBeforeMutation`; no text is claimed to have changed.

### 5.3 Begin / Update / Commit / Cancel rules

* **Begin** requires no active composition, a collapsed selection exactly at the plan's
  expected caret, and a verified scalar boundary. It starts a composition at that caret and
  inserts the preedit inside that composition. It does not replace the document.
* **Update** requires an active composition. It reads the composition range back and refuses
  (`CompositionRangeMismatch`) if the range no longer holds the text this writer last wrote.
  Otherwise it replaces the composition text; h → he → hel changes the range length, not the
  document length by accumulation.
* **Commit** with an active composition replaces the composition text and ends the
  composition. Without a composition, it inserts the committed text once at the verified
  collapsed caret via `ITfInsertAtSelection::InsertTextAtSelection`; an empty commit with no
  composition is refused (`NothingToCommit`). TSF wraps the insertion in a temporary
  composition of its own (observed start/end counts balanced); that is not the layer's
  composition.
* **Cancel** requires an active composition, verifies its text, replaces it with the empty
  string, and ends it. It never deletes text the writer does not own.
* After a successful `EndComposition`, the composition and sink references are released and
  the object is never used again. A subsequent Update/Cancel is `NoComposition`.
* If TSF terminates the composition from the owner side, the per-composition
  `ITfCompositionSink::OnCompositionTerminated` records it; the next Apply releases the
  terminated objects and returns `Rejected` (`CompositionTerminated`) without calling a method
  on them. This path is implemented but not exercised by the current host tests.

### 5.4 `StartComposition` and the composition sink

On the Windows 11 10.0.26200 / msctf build used here, `StartComposition` with a **NULL**
`ITfCompositionSink*` returned `E_INVALIDARG` (`0x80070057`) even for a valid collapsed ACP
range inside a granted write session. The same call with a real sink returned `S_OK` and a
composition object. Microsoft's documentation describes `pSink` as optional; the observed
implementation is stricter. The layer therefore always supplies its own sink. This is a
verified environment fact, not a documentation claim.

If `StartComposition` returns `S_OK` with a NULL composition (the documented context-owner
refusal), the layer returns `Rejected` (`CompositionRejected`) and writes no text. The host
case `composition_rejected` shows `starts=1 refusals=1 ends=0 set_text_calls=0`.

### 5.5 Read-back and partial failure

For an ordinary mutating-call failure, the writer reads the requested document span back
through TSF: `ITfContext::GetStart`/`GetEnd`, `ITfRange::ShiftEndToRange(TF_ANCHOR_START)`,
and `ITfRange::GetText`, then compares the UTF-16 slice. The read is bounded at 1 Mi UTF-16
code units; a larger document is reported unreadable rather than copied without limit. If
the slice is the old text, the result is `FailedBeforeMutation` with
`mutation_verified = true`; if it is the new text, `FailedAfterMutation` verified; if it
cannot be read, `FailedAfterMutation` with `text_may_have_changed = true` and
`mutation_verified = false` (conservative, no "unchanged" claim).

`E_UNEXPECTED` is checked before the read-back and means "the store says it partially
applied"; the result is `FailedAfterMutation`, `mutation_verified = false`.

After a failed **Begin**, the layer ends its composition without another SetText and leaves
any mutated text in place. This is not a rollback and does not replay the insert. Observed:
`before_status=FailedBeforeMutation before_verified=1 after_status=FailedAfterMutation
after_may_change=1 after_verified=0 mutations=1 end_calls=2`.

`EndComposition` failure after a successful SetText is implemented as
`FailedAfterMutation` with `end_composition_hr` and no rollback/replay, but the host has no
way to induce a TSF-side `EndComposition` failure; it is not covered by a test.

## 6. Identity, generation, permission, read-only

* A plan is applied only if its `ContextIdentity` equals the bound identity. A different
  context id is `Stale`/`IdentityMismatch`; an older generation is
  `Stale`/`StaleGeneration` or `NotBound` after `Invalidate`.
* `Invalidate` increments the generation and unbinds. The callback rechecks
  closed/bound/identity/generation/permission and the exact `ITfContext*` under the state
  mutex before making any TSF call. An asynchronous plan queued before a generation change is
  `Stale` and writes nothing (observed: `status=Stale reason=NotBound mutations=0
  composition_starts=0`).
* Two contexts in one ThreadMgr are independent: a plan for A is `Stale` on B; neither text
  is affected by the other's edits.
* `InputPermission` is host-supplied: `Allowed`, `Denied`, `Unknown`. The executor writes
  only for `Allowed`; `Denied`/`Unknown` are refused before `RequestEditSession`, so no lock
  is requested and no text is touched. This is **not** real password-field detection.
* Read-only is checked twice: `ITfContext::GetStatus` (`TF_SD_READONLY`) before submission
  (observed `reason=ReadOnlyContext request_locks_before=0 after=0`) and the store's own
  read/write-lock refusal (`refuse_write_locks`; observed
  `reason=WriteLockRefused refusals_after=1 mutations=0`).
* A context popped from its document stack is refused (`ContextDisconnected`); the host's
  `Pop` now actually runs (see 3.5).

## 7. Failure semantics

* No automatic retry.
* No rollback claim after a mutation.
* No automatic replay of a commit.
* `SetText` success + later step failure is `FailedAfterMutation` with the later HRESULT; the
  text is not claimed to be reverted.
* A failed Begin ends its composition; a failed Update/Commit/Cancel keeps the composition
  reference (unless TSF terminated it) so the caller can decide. A later operation on a range
  whose text no longer matches is refused rather than deleting unknown text.
* Status counters (`applied`, `rejected`, `failed_before_mutation`,
  `failed_after_mutation`, `stale`, `callbacks`, `locks_observed`) are recorded in
  `CompositionEditSession::Snapshot()`.

## 8. Verification

Environment: Windows 10.0.26200 x64, Visual Studio 17 2022 17.14,
MSVC 19.44.35209, CMake/CTest 3.31.6-msvc6, Windows SDK 10.0.26100.0. Static CRT, x64,
Release configuration for the platform projects.

### 8.1 Build and run

```text
cmake -S platforms/windows/tsf_edit -B build/p37-tsf-edit
      -G "Visual Studio 17 2022" -A x64 -DCMAKE_CONFIGURATION_TYPES=Release
cmake --build build/p37-tsf-edit --config Release
ctest --test-dir build/p37-tsf-edit -C Release --output-on-failure --no-tests=error
```

Result: **100% tests passed, 0 failed out of 24** (local time 10.5 s). The 24 cases are:

```text
windows.tsf_edit.host_sync_edit
windows.tsf_edit.host_async_edit
windows.tsf_edit.host_unsupported_surface
windows.tsf_edit.host_cleanup
windows.tsf_edit.lock_callback
windows.tsf_edit.update_replaces_composition
windows.tsf_edit.commit_no_duplicate
windows.tsf_edit.cancel_only_own
windows.tsf_edit.ended_composition_unused
windows.tsf_edit.no_preedit_commit
windows.tsf_edit.unicode_boundaries
windows.tsf_edit.surrogate_offset_rejected
windows.tsf_edit.read_only_rejected
windows.tsf_edit.write_lock_refused
windows.tsf_edit.disconnected_rejected
windows.tsf_edit.async_pending
windows.tsf_edit.stale_generation
windows.tsf_edit.two_contexts
windows.tsf_edit.failure_before_after
windows.tsf_edit.permission_denied_unknown
windows.tsf_edit.composition_rejected
windows.tsf_edit.resource_cleanup
windows.tsf_edit.composition_can_fail
windows.tsf_edit.host_can_fail
```

The two `*_can_fail` cases are falsifiability controls: each makes one deliberately false
claim, must exit 14, and must not print `RESULT PASS`.

### 8.2 Required behaviours mapped to cases

| # | Requirement | Case(s) |
| --- | --- | --- |
| 1 | real lock and edit callback | `lock_callback` |
| 2 | text around the caret; updates only the composition range | `update_replaces_composition` |
| 3 | h → he → hel replaces, not appends | `update_replaces_composition` |
| 4 | Commit leaves no duplicate word | `commit_no_duplicate` |
| 5 | Cancel removes only its own composition | `cancel_only_own` |
| 6 | an ended composition object is not reused | `ended_composition_unused` |
| 7 | a no-preedit commit inserts once | `no_preedit_commit` |
| 8 | Unicode prefix/suffix survive | `unicode_boundaries` |
| 9 | an offset inside a surrogate pair is refused | `surrogate_offset_rejected` |
| 10 | read-only refusal; write-lock refusal | `read_only_rejected`, `write_lock_refused` |
| 11 | disconnected/invalid context refused | `disconnected_rejected` |
| 12 | async is Pending until the callback | `async_pending` |
| 13 | generation change before callback → no write | `stale_generation` |
| 14 | two contexts do not cross-talk | `two_contexts` |
| 15 | failure before vs after mutation | `failure_before_after` |
| 16 | denied/unknown permission writes nothing | `permission_denied_unknown` |
| 17 | resources, sink, composition, context, ThreadMgr cleanup | `host_cleanup`, `resource_cleanup` |

Additional coverage: `composition_rejected` (StartComposition `S_OK` + NULL),
`host_sync_edit` / `host_async_edit` (the host's own real-TSF path), and
`host_unsupported_surface` (honest refusals).

### 8.3 Evidence lines

The delivered logs contain, among others:

```text
evidence lock-callback: request_lock=1 write_locks=1 mutations=1 callbacks=1 locks_observed=1
evidence update-scope: composition extents [3,1) -> [3,2) -> [3,3); mutations=3; prefix/suffix intact
evidence commit-no-duplicate: hello occurrences=1 mutations=2 end_calls=1
evidence async-pending: pending_status=Pending final_status=Applied request_hr=0x00000000 phr=0x00040300 callbacks=1 async_grants=1
evidence stale-generation: status=Stale reason=NotBound mutations=0 composition_starts=0
evidence read-only: reason=ReadOnlyContext request_locks_before=0 after=0 get_status_calls=1
evidence write-lock-refused: reason=WriteLockRefused refusals_before=0 after=1 mutations=0
evidence failure-before-after: before_status=FailedBeforeMutation before_verified=1 after_status=FailedAfterMutation after_may_change=1 after_verified=0 mutations=1 end_calls=2
evidence composition-rejected: reason=CompositionRejected starts=1 refusals=1 ends=0 set_text_calls=0
evidence resource-cleanup: store_refs=0 live_stores=0 sink_before=1 sink_after_pop=0 addref=4 release=4 composition_ends=1
```

### 8.4 Regressions

All from the current source tree, with new log tags (no log was overwritten):

| Suite | Result |
| --- | --- |
| P3.1 TSF lifecycle (`build/windows-tsf/test`, Release) | 3/3 |
| P3.2 text encoding/boundary (`build/windows-text/test`, Release) | 4/4 |
| P3.3 input decisions/translation (`build/windows-input/test`, Release) | 5/5 |
| P3.4–P3.6 bridge/executor/hosted/broker (`build/windows-engine-bridge/test`, Release) | 22/22 |
| Root project infrastructure/English core (`build/vs2022-x64-debug`, Debug) | 8/8 |
| Formal English entry (`tools/test_english_integration.ps1`, new build dir + tag) | 29/29 |

Formal English entry details: the project built is
`tools/rime_english_recovery`; the EXEs are
`build/p37-formal-english/english-engine/rime_english_recovery.exe`,
`.../baseline-engine/rime_english_recovery_baseline.exe`, and
`.../sha256-helper/rime_english_recovery_sha256.exe`. The script exported and cross-checked
the CTest list against the `NAME` registrations in `CMakeLists.txt`; the actual count is 29
(26 engine cases + 3 helper cases), with `-PilotLexicon` unset. The earlier `^english. 15/15`
description is **not** used; the counts here come from this source and this generated list.

## 9. Unimplemented items and known limits

1. **No installed input method.** No COM registration, no `AdviseKeyEventSink`, no candidate
   UI, no external application was touched. A green suite does not mean an IME is available.
2. **No real password/one-time-code detection.** Input permission is an explicit host gate.
3. **Focus is unverified.** `ITfThreadMgr::SetFocus` returned `E_FAIL` in this headless
   process; `RequestEditSession` works without it. `AssociateFocus` and a focused-window
   round trip were not tested.
4. **External composition termination** is detected via `ITfCompositionSink` but not exercised
   by a test. The host never terminates a composition from the owner side.
5. **EndComposition failure after SetText** is implemented as `FailedAfterMutation` but cannot
   be induced without a TSF-side fault seam, which this phase deliberately does not add.
6. **A silent partial mutation with a plain `E_FAIL` cannot be observed through TSF.** TSF
   keeps its cached text; the controlled host uses `E_UNEXPECTED` to declare partial
   application. A store that mutates and lies with `E_FAIL` is outside this layer's
   verification (the layer would read the old TSF text and classify `FailedBeforeMutation`).
7. **Read-back is capped at 1 Mi UTF-16 code units.** Beyond that the layer reports
   `FailedAfterMutation` unverified rather than reading an unbounded document.
8. **Update/Commit/Cancel partial failure** keeps a possibly stale composition instead of
   auto-ending it; a later operation detects the range mismatch and refuses. Only Begin
   auto-ends its failed composition.
9. **StartComposition with a NULL sink** is rejected by this msctf build despite the SDK
   documentation calling the sink optional; the layer always supplies one.
10. **No grapheme editing**, no candidate window, no key handling, no automatic retry, no
    rollback.
11. **Stage-1 host defect:** the `connected_` bug described in 3.5 was committed in
    `b208184`; the correction is in commit 2. The stage-1 cleanup green was not sufficient
    evidence and is superseded by the corrected zero-reference result.

## 10. Sources checked

SDK headers (installed Windows SDK 10.0.26100.0):

* `um/textstor.h`: `TS_E_*`/`TS_S_ASYNC` 110–121; `TS_LF_*` 134–138; `TS_SD_*` 140–156;
  `ITextStoreACP` 306–470; `ITextStoreACPSink` 1303–1330.
* `um/msctf.h`: `TF_S_ASYNC` 749; `TF_ES_*` 3362–3370; `ITfContext::RequestEditSession`
  3419; `ITfEditSession::DoEditSession` 5635; `ITfRange::SetText` 5759;
  `ITfRangeACP` 6156; `ITfComposition::EndComposition` 2559;
  `ITfContextComposition::StartComposition` 2755;
  `ITfContextOwnerCompositionSink` 3010; `ITfSource` 11662.

Microsoft Learn pages used for the behavioural rules (not memory):

* ITextStoreACP::RequestLock; ITextStoreACPSink::OnLockGranted; Document Locks;
  ITextStoreACP::GetText/SetText/SetSelection/InsertTextAtSelection/QueryInsert/GetEndACP;
  ITfContext::RequestEditSession; ITfContextComposition::StartComposition;
  ITfComposition::EndComposition; ITfContextOwnerCompositionSink; ITfRange::SetText;
  ITfRangeACP::SetExtent; ITfThreadMgr::SetFocus; ITfDocumentMgr::CreateContext/Pop.

Observed deviations from those pages are stated in 3.4/3.5 and 5.4 rather than smoothed over.
