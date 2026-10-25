# EpoCanva P3.9 — real TSF input-permission contract

This document describes what `platforms/windows/tsf_context/` resolves, how the
coordinator uses it, what was actually verified against the real TSF manager, and what
the result does **not** guarantee. It is written after the code and the test runs, not
from an earlier design.

Phase staging, as committed:

| Commit | Message | Content |
| --- | --- | --- |
| `bc9a53c6836690962e172b3aeb3855b90297d9b7` | `feat(windows): resolve input permission from TSF context` | the pure classifier, the real read-only query source, the P3.7 host metadata knobs, and the `tsf_context` suite |
| second commit (SHA in the delivery report) | `feat(windows): gate engine dispatch on context permission` | coordinator admission, the `PermissionPending` phase, apply-time and callback-time re-verification, divergence handling, real-metadata integration cases |

## 1. Scope

In scope:

* classify one bound TSF context as `Allowed`, `Denied` or `Unknown` from real metadata;
* read `ITfContext::GetStatus`, the `GUID_COMPARTMENT_KEYBOARD_DISABLED` and
  `GUID_COMPARTMENT_EMPTYCONTEXT` context compartments, and the
  `GUID_PROP_INPUTSCOPE` application property;
* use only read-only edit sessions for the property read, never a fabricated cookie and
  never the cookie returned by `CreateContext`;
* gate coordinator engine dispatch and edits on the verdict, including a re-read at the
  edit callback with the cookie actually granted;
* handle the engine/host divergence when the engine already consumed an event but the
  edit was refused;
* prove all of the above through the real TSF manager in the P3.7 test host.

Out of scope and **not claimed**:

* no input-method registration, no `AdviseKeyEventSink`, no candidate UI, no language
  bar, no focus tracking, no external cursor/window scanning;
* no reading of document text, clipboard, UIA, OCR or any other application content;
* no guarantee that an application publishes truthful or complete InputScope metadata;
* no claim that an `Allowed` verdict makes a field safe for arbitrary content;
* no network I/O anywhere in the runtime path (see `AGENTS.md` §2).

## 2. Decision model

```cpp
enum class PermissionState : uint8_t { kAllowed, kDenied, kUnknown };
```

| Verdict | Meaning | What callers may do |
| --- | --- | --- |
| `Allowed` | every axis was queried, every answer was interpretable, and none blocked | dispatch to the engine; request an edit |
| `Denied` | at least one axis produced hard evidence that this context must not receive input | no engine call, no edit |
| `Unknown` | at least one axis could not be interpreted (read failure, wrong VARIANT type, unrecognized scope value, incomplete query) | no engine call, no edit |

Precedence: **Deny outranks Unknown** (a known refusal stays reportable beside an
unreadable axis). `Unknown` is never a placeholder for "probably fine"; only a complete,
interpretable, non-blocking query can produce `Allowed`.

Findings are bit flags, text-free (see `input_permission.h`): `ReadOnlyContext`,
`KeyboardDisabled`, `EmptyContext`, `SensitiveInputScope`, and the unknown-side
`StatusQueryFailed`, `KeyboardDisabledQueryFailed`, `EmptyContextQueryFailed`,
`ScopeQueryFailed`, `ScopeWrongValueType`, `UnrecognizedScopeValue`.

## 3. What is read, exactly

All reads happen on the STA thread that owns the context, inside the bound `ITfContext`.

| Axis | Real call chain | Cookie needed | Expected value | No-value / failure semantics |
| --- | --- | --- | --- | --- |
| document status | `ITfContext::GetStatus` (`msctf.h:3455`) | no | `TF_STATUS.dwDynamicFlags`; `TF_SD_READONLY` (`msctf.h:3394`, `textstor.h:140`) | failure → `StatusQueryFailed` → `Unknown` |
| keyboard disabled | `ITfContext` → `QI(IID_ITfCompartmentMgr)` → `GetCompartment(GUID_COMPARTMENT_KEYBOARD_DISABLED)` → `ITfCompartment::GetValue` | no | `VT_I4` `DWORD`; “nonzero if the keyboard is disabled or zero otherwise” | `S_FALSE` + `VT_EMPTY` = no value; wrong type / failure → `Unknown` |
| empty context | same chain with `GUID_COMPARTMENT_EMPTYCONTEXT` | no | `VT_I4` `DWORD`; “nonzero if the context is empty or zero otherwise” | same as above |
| InputScope | `ITfContext::GetAppProperty(GUID_PROP_INPUTSCOPE)` (`msctf.h:3462`) → `ITfReadOnlyProperty::GetValue(ec, range, &var)` (`msctf.h:4109`) → `QI(IID_ITfInputScope)` → `GetInputScopes` | **yes**; a real read-only edit session | `VT_UNKNOWN` holding `ITfInputScope` | `S_FALSE`/`E_NOTIMPL` from `GetAppProperty` = property not supported; `VT_EMPTY`/`VT_NULL` = no value; other failures → `Unknown`; `VT_UNKNOWN` without `ITfInputScope` → `ScopeWrongValueType` → `Unknown` |

Citations:

* `ITfCompartment::GetValue` return values: `S_FALSE` means “The compartment has no
  value. `pvarValue` receives `VT_EMPTY`”; `E_UNEXPECTED` means the compartment was
  cleared. <https://learn.microsoft.com/en-us/windows/win32/api/msctf/nf-msctf-itfcompartment-getvalue>
* Predefined compartments: both `GUID_COMPARTMENT_EMPTYCONTEXT` and
  `GUID_COMPARTMENT_KEYBOARD_DISABLED` are `VT_I4` `DWORD`s whose meaning is
  “nonzero if … or zero otherwise”.
  <https://learn.microsoft.com/en-us/windows/win32/tsf/predefined-compartments>
* `ITfContext::GetAppProperty`: `S_FALSE` = “The context owner does not support the
  property”; `E_NOTIMPL` = “The context owner does not implement this method”; the value
  is maintained by the context owner and cannot be modified by a text service.
  <https://learn.microsoft.com/en-us/windows/win32/api/msctf/nf-msctf-itfcontext-getappproperty>
* `ITfInputScope`: the text service obtains it with `GetAppProperty` +
  `ITfReadOnlyProperty::GetValue(ec, range, …)` and `QueryInterface(IID_ITfInputScope)`;
  `GetInputScopes` returns a `CoTaskMemAlloc`'d array that the caller frees with
  `CoTaskMemFree`.
  <https://learn.microsoft.com/en-us/windows/win32/api/inputscope/nn-inputscope-itfinputscope>,
  <https://learn.microsoft.com/en-us/windows/win32/api/inputscope/nf-inputscope-itfinputscope-getinputscopes>

SDK headers checked on disk (Windows SDK 10.0.26100.0):
`um/inputscope.h` (scope constants 106–179, `GUID_PROP_INPUTSCOPE` 200),
`um/msctf.h` (`GUID_COMPARTMENT_*` 809/816, `ITfContext::GetStatus` 3455,
`RequestEditSession` 3419, `GetSelection` 3429, `GetAppProperty` 3462,
`ITfReadOnlyProperty::GetValue` 4109, `ITfCompartment` 7510, `ITfCompartmentMgr` 7691),
`um/textstor.h` (`TS_SD_READONLY` 140, `TS_E_*` 111–119).

### 3.1 Nonzero is not `TRUE`

`KEYBOARD_DISABLED` and `EMPTYCONTEXT` are documented as “nonzero if …”, not as
`VARIANT_TRUE`. The classifier tests `value != 0` for the whole `LONG` range. The suite
drives `1`, `2`, `0x40000000` and `LONG_MIN` through the real `SetValue` path; all deny.
`0` does not deny; a missing value (official `S_FALSE`/`VT_EMPTY`) does not deny.

### 3.2 Four different absences

| Observation | Classification | Rationale |
| --- | --- | --- |
| compartment `S_FALSE` + `VT_EMPTY` | absent → does not block | official “compartment has no value”; for these predefined compartments that is the same as zero |
| compartment `S_OK` + `VT_EMPTY`/`VT_NULL` | absent | a successful read with no payload; same meaning |
| compartment `S_OK` + non-`VT_I4` | `Unknown` | uninterpretable, never “false” |
| compartment failure (including `E_UNEXPECTED`) | `Unknown` | not evidence of zero |
| `GetAppProperty` `S_FALSE` / `E_NOTIMPL` | scope absent → does not block | documented “does not support / does not implement” |
| property `VT_EMPTY` / `VT_NULL` | scope absent | no value |
| property non-`VT_UNKNOWN` / no `ITfInputScope` | `Unknown` | malformed metadata |
| `GetSelection` failure inside the read session | `Unknown` (scope axis query failure) | the range read could not run |
| other `GetValue` failure | `Unknown` | not a no-value answer |

A cleared compartment is recreated by the next `GetCompartment` call on this build, so a
fresh query sees `S_FALSE`/`VT_EMPTY` and resolves Allowed; the reader still maps
`E_UNEXPECTED` from a retained pre-clear reference to `Unknown`. The `tsf_context` suite
pins the recreated behaviour (`cleared_compartment_recreated`).

## 4. InputScope policy

### 4.1 Sensitive set

The pinned header defines these named values; the policy denies all of them:

| Constant | Value | Why denied |
| --- | --- | --- |
| `IS_PASSWORD` | 31 | password entry |
| `IS_PRIVATE` | 61 | private data |
| `IS_NUMERIC_PASSWORD` | 63 | numeric password |
| `IS_NUMERIC_PIN` | 64 | numeric PIN |
| `IS_ALPHANUMERIC_PIN` | 65 | alphanumeric PIN |
| `IS_ALPHANUMERIC_PIN_SET` | 66 | alphanumeric PIN set |

`IS_ONE_TIME_CODE` **does not exist** in `um/inputscope.h` 10.0.26100.0 and is not
invented, guessed, or inferred from the input text. Ordinary values such as `IS_DEFAULT`
(0), `IS_URL`, `IS_SEARCH`, `IS_EMAIL_SMTP_URL` are not sensitive; the documented range
is `IS_ENUMSTRING = -5` … `IS_CHAT_WITHOUT_EMOJI = 68`. A value outside that range is
`UnrecognizedScopeValue` → `Unknown`, not silently treated as harmless.

### 4.2 List semantics

* One sensitive value anywhere in the list denies the whole list.
* Order does not matter; the tests put the sensitive value first, in the middle, and last.
* An explicitly empty `GetInputScopes` array (`S_OK`, count 0) is an empty list, not a
  failure; it does not block.
* A `VT_UNKNOWN` payload without `ITfInputScope` is a wrong value type → `Unknown`.
* A recognized scope value outside the pinned header's range → `Unknown`.

### 4.3 The honest limit

InputScope is **application-provided metadata**. TSF does not validate it and cannot
force a non-TSF-aware or malicious application to publish `IS_PASSWORD`. Therefore:

* an `Allowed` verdict means “no known blocking metadata was observed”, not “this field
  is safe”;
* absence of `IS_PASSWORD` is **not** evidence of a non-password field — it is the
  documented default for ordinary fields;
* EpoCanva does not infer a password or one-time-code field from text, clipboard, UIA,
  OCR, window titles or external scanning; doing so would violate `AGENTS.md` §2 and
  would be a guess, not a permission fact.

## 5. Query lifetime, identity and threading

`PermissionSource` (production: `CreateRealPermissionSource`) is a one-active-query
object:

* `Begin(context, context_id, generation)` allocates a fresh per-query state. A query
  begun against `(context, context_id, generation)` carries that triple in its result.
* The InputScope read requests `TF_ES_READ`; a synchronous grant is tried first, and
  `TF_E_SYNCHRONOUS` falls back to `TF_ES_ASYNC`. `TF_S_ASYNC` means **pending**; the
  host pumps the real message loop and waits on the source's completion event.
* A callback that never runs cannot make a caller wait forever: `Abandon()` returns
  immediately, signals the completion event, releases the held context reference, and
  drops the state. A late callback keeps its own state alive and publishes nothing.
* `Result()` is meaningful only in `kReady`; the coordinator compares context id and
  generation again at every use. A verdict from another context or generation never
  grants.
* `Abandon()` on an already-completed query is not counted as an abandoned query; the
  diagnostics distinguish “never answered” from “answered”.
* A completed query releases its `ITfContext` reference immediately; the source does not
  pin the document between queries.
* All methods run on the context's STA thread. No `ITfContext`, property, range or
  `ITfInputScope` pointer is sent to the engine thread.

Resource rules, per the SDK contracts:

* the `TF_SELECTION` range is released after the value is read;
* the `VT_UNKNOWN` payload reference is released through `VariantClear`;
* the `InputScope` array is freed with `CoTaskMemFree`;
* `ITfReadOnlyProperty` and `ITfCompartment` are released; no borrowed pointer is cached;
* the completion event is a manual-reset event owned by the query and closed with its
  state.

## 6. Coordinator admission

### 6.1 Gate order

`SubmitKey` / `SubmitCandidate` check, in order:

1. closed;
2. a work item is unresolved → `Busy` (single flight outranks everything below);
3. not bound;
4. block latch (uncertain / needs-resync / unsupported shape);
5. **real permission query**;
6. local pure candidate evaluation;
7. executor admission.

If the query completes synchronously, a Denied/Unknown verdict returns
`PermissionDenied`/`PermissionUnknown` and no work item exists. If it goes async, the
work is accepted into `WorkPhase::kPermissionPending`; no engine call and no edit exist
for it yet. `Pump()` advances only after the real read session completed; `Close()` or
`InvalidateBinding()` terminalizes it immediately as `AbandonedStale` and abandons the
query, so a callback that never runs cannot hang the coordinator.

### 6.2 One work item

```
Idle --Submit--> [PermissionPending] --Allowed--> EnginePending
     --Pump/TakeResult--> decide plan
     --edit-producing: apply-time permission query (may be PermissionPending)
     --Allowed--> EditPending --callback--> Applied | NoEdit | NeedsResync | ...
```

* The **submit-time** query happens before the engine. `Unknown`/`Denied` never dispatch.
* The **apply-time** query happens only when the observation produces an edit
  (Begin/Update/Commit/Cancel). It uses current real metadata, not the submit-time cache.
* The **callback-time** recheck happens inside `CompositionEditSession::DoEditSession`,
  with the granted edit cookie, immediately before any TSF write. It calls
  `ReadPermissionFactsWithCookie` (same four axes, no new edit session) and is
  authoritative. A permission change while the lock was deferred therefore refuses the
  write even though the pre-edit query said Allowed. A throw or `Unknown` refuses; it is
  never treated as Allowed.
* The P3.7 session's cached `InputPermission` is kept in step with every verdict as
  defense in depth, but the callback-time real-metadata check is the last word.

### 6.3 Engine/host divergence

Once the engine has consumed an event, the engine's internal state and the document may
have diverged. The coordinator therefore distinguishes:

| Situation | Terminal |
| --- | --- |
| submit-time query Denied/Unknown (no engine call) | `Rejected`, no work item |
| dispatch-time async query ends Denied/Unknown | `Rejected` (`PermissionDenied`/`PermissionUnknown`), no engine call |
| engine command could not be dispatched (client unavailable / invalid request) | `Rejected` (`EngineUnavailable` / `EngineSubmitFailed`) |
| engine ran, result unreadable | `NeedsResync` |
| engine ran, apply-time permission now Denied/Unknown | `NeedsResync` (`PermissionChangedWhileQueued`); no edit requested |
| engine ran, edit callback refused (read-only, refused lock, callback-time permission, disconnected) | `NeedsResync` (`EditRefused` / `EditFailedBeforeMutation`) |
| engine ran, edit half-applied / unconfirmed | `Uncertain`; context-id lockdown |
| engine ran, commit + live preedit in one observation | `UnsupportedShape`; lock down |

`NeedsResync` and `UnsupportedShape` block further normal input for that binding. The
only way out is an explicit `InvalidateBinding()` plus a rebuilt binding; the host owns
rebuilding the engine client. There is no automatic replay, no automatic rollback, no
attaching an old commit to a new context, no automatic clearing of unknown text, and no
silent `Close` + new coordinator to bypass an uncertain text state.

`AbandonedStale` is different: the binding itself was invalidated/closed, so the old
result is discarded and never applied anywhere. The host must not reuse the old engine
client's composition state for the new binding.

### 6.4 Plan-decision order

The pure mapping (`engine_to_plan.cc`) now makes the “did the engine run?” distinction
before interpreting the observation:

1. no engine call at all → `NoEdit`;
2. a call ran but state/commit is unreadable → `InvalidObservation` (never `NoEdit`,
   even if the engine also reported the call unhandled);
3. a call ran, the observation is readable, engine reported unhandled/refused → `NoEdit`;
4. commit + live preedit → `UnsupportedShape`;
5. commit / update / begin / cancel / no-edit as before.

This specifically prevents a dispatched-but-unreadable result from being hidden by an
early “NoEdit” branch. The coordinator’s `BeginApplyAfterEngine` then decides the plan
before starting the apply-time permission query, so a permission answer cannot mask an
unreadable engine result either.

## 7. Evidence from the real tests

All cases use `CreateRealPermissionSource` against the P3.7 host; none hands the
coordinator an Allowed value. The engine is the controlled scheduling backend, so
`engine_calls` and `mutations` are direct host counters.

| # | Scenario | Case | Observed |
| --- | --- | --- | --- |
| 1 | ordinary editable context | `permission_real_allow` | 3 works applied; 6 queries, 3 callback rechecks; `engine_calls=3` |
| 2 | read-only | `permission_real_deny` (read-only) | `PermissionDenied`, `engine_calls=0`, `mutations=0` |
| 3 | keyboard-disabled 1 / 2 / `LONG_MIN` | `permission_real_deny` | `PermissionDenied`, `engine_calls=0`, `mutations=0` for every value |
| 4 | empty context | `permission_real_deny` | `PermissionDenied`, `engine_calls=0`, `mutations=0` |
| 5 | sensitive scope (31/61/63/64/65/66) | `permission_real_deny` | `PermissionDenied`, `engine_calls=0`, `mutations=0` |
| 6 | one sensitive item in a multi-scope list | `permission_real_deny` | `PermissionDenied`, `engine_calls=0`, `mutations=0` (first/middle/last) |
| 7 | scope absent / empty / query failure / wrong type | `permission_real_unknown`, `scope_absent_wrongtype_fail` | absent and empty → Allowed/work; wrong type and selection-read failure → `Unknown`, `engine_calls=0`; owner `E_FAIL` is documented as indistinguishable from absence |
| 8 | unknown permission does not dispatch | `permission_real_unknown` | `unknown` shapes: `engine_calls=0`, `mutations=0` |
| 9 | async permission read, no dispatch before completion | `permission_real_async` | `PermissionPending`, event exposed, `engine_calls=0` until pumped; then applied; `async_reads=2` |
| 10 | Allowed → Denied, edit does not happen | `permission_real_flip_before_apply` | `NeedsResync`, `PermissionChangedWhileQueued`, `engine_calls=1`, `write_lock_grants=0`, `mutations=0`, gate `BlockedNeedsResync` |
| 11 | engine ran, edit refused, restoring Allowed must not surface old text | `permission_real_engine_ran_refused` | engine holds `"h"`; callback recheck refuses (`rechecks=1`); `mutations=0`; after restore `Submit` is `BlockedNeedsResync`; fresh binding/document gets only `"a"` |
| 12 | binding generation change invalidates old verdict | `permission_real_generation` | old work `AbandonedStale`, old query `Idle`, no dispatch; new binding's own async verdict allows only its own key; `queries_abandoned=1`; generation seam rejects a verdict stamped `generation+1` |
| 13 | Close while permission pending | `permission_real_close_pending` | `Close` terminal immediately; `abandoned=1`, `completed=0`; late grant publishes nothing; `engine_calls=0`, `mutations=0` |
| 14 | resource balance | `permission_real_balance` | 3 denied queries and completed verdicts release the context; after host shutdown `stores=0`, `scopes=0`, zero store refs |
| 15 | same condition repeated | `permission_real_allow`, `repeated_queries_consistent` | every repeated query yields the same decision; `queries_completed == queries_begun` |

Additional parser-level cases: `pure_classification`, `real_path_probe`,
`plain_editable_allowed`, `read_only_denied`, `keyboard_disabled_values`,
`empty_context_denied`, `sensitive_scopes_denied`, `scope_absent_wrongtype_fail`,
`cleared_compartment_recreated`, `repeated_queries_consistent`, `async_query_pending`,
`abandon_pending_query`, `resource_balance` (14 cases including the falsifiability
control).

### 7.1 Evidence lines (text-free, from the delivered direct runs)

```text
evidence permission-allow: engine_calls=3 queries=6 rechecks=3
evidence permission-refusal read-only: submit=PermissionDenied state=Denied engine_calls=0 mutations=0
evidence permission-refusal keyboard-disabled-1: submit=PermissionDenied state=Denied engine_calls=0 mutations=0
evidence permission-refusal keyboard-disabled-2: submit=PermissionDenied state=Denied engine_calls=0 mutations=0
evidence permission-refusal keyboard-disabled--2147483648: submit=PermissionDenied state=Denied engine_calls=0 mutations=0
evidence permission-refusal empty-context: submit=PermissionDenied state=Denied engine_calls=0 mutations=0
evidence permission-refusal sensitive-scope-31: submit=PermissionDenied state=Denied engine_calls=0 mutations=0
evidence permission-refusal sensitive-list-0: submit=PermissionDenied state=Denied engine_calls=0 mutations=0
evidence permission-refusal wrong-type: submit=PermissionUnknown state=Unknown engine_calls=0 mutations=0
evidence permission-refusal scope-query-failure: submit=PermissionUnknown state=Unknown engine_calls=0 mutations=0
evidence permission-refusal disconnected: submit=PermissionUnknown state=Unknown engine_calls=0 mutations=0
evidence gap scope-owner-failure: E_FAIL-is-indistinguishable-from-absent
evidence async-permission: engine_calls=1 async_reads=2 mutations=1
evidence flip-before-apply: engine_calls=1 write_lock_grants=0 mutations=0 gate=BlockedNeedsResync
evidence engine-ran-edit-refused: old_engine="h" old_doc_mutations=0 fresh_engine="a" rechecks=1
evidence generation: old_abandoned=1 new_engine_calls=1 old_doc_mutations=0
evidence close-permission: abandoned=1 completed=0 engine_calls=0 mutations=0
evidence balance: stores=0 scopes=0
```

## 8. Build and test

Environment: Windows 10.0.26200 x64; Visual Studio 17 2022 (MSVC 19.44); CMake/CTest
3.31.6-msvc6; Windows SDK 10.0.26100.0; static CRT; x64 Release for the engine-bridge
tree. No network access is used by any runtime code.

### 8.1 Commands actually run

```text
cmake -S platforms/windows/engine_bridge -B build/windows-engine-bridge/test
cmake --build build/windows-engine-bridge/test --config Release
ctest --test-dir build/windows-engine-bridge/test -C Release --output-on-failure --no-tests=error

cmake --build build/p37-tsf-edit --config Release
ctest --test-dir build/p37-tsf-edit -C Release --output-on-failure --no-tests=error

# P3.1/P3.2/P3.3, each its own tree
cmake --build build/windows-tsf/test --config Release
ctest --test-dir build/windows-tsf/test -C Release --output-on-failure --no-tests=error
# ... windows-text/test and windows-input/test the same way

cmake --build build/vs2022-x64-debug --config Debug
ctest --test-dir build/vs2022-x64-debug -C Debug --output-on-failure --no-tests=error

powershell -NoProfile -ExecutionPolicy Bypass -File tools/test_english_integration.ps1 \
  -EnginePrefix build/librime-english/rime/install \
  -BaselinePrefix build/librime-baseline/rime/install \
  -BuildDir build/p39-formal-english -LogTag p39-r2
```

### 8.2 Results

| Suite | Result |
| --- | --- |
| engine-bridge full tree (P3.4–P3.6, P3.7 host layer, P3.8 coordinator, P3.9 context + permission cases) | **77/77 passed** |
| `windows.tsf_context.*` | 14/14 |
| `windows.tsf_coordinator.*` (stage-1 14 + permission 9 + real-engine 18) | 41/41 |
| P3.7 standalone edit layer | 29/29 |
| P3.1 TSF lifecycle | 3/3 |
| P3.2 text | 4/4 |
| P3.3 input | 5/5 |
| Formal English entry (`tools/test_english_integration.ps1`) | 29/29 |
| Root project, Debug | 7/8 — `english.core.spelling_cost_trial_on_pilot_lexicon` measured 64,473.5 µs against the unchanged 25,000 µs Debug budget; reported separately, no threshold changed |

The Debug performance case is the known pre-existing English-core timing failure; this
round changed no English-core source and recompiled none of that binary. It is listed as
a failure, not hidden.

### 8.3 Resource evidence

`permission_real_balance` asserts, after the host's teardown:

* `TestTextStore::LiveInstanceCount() == 0`;
* `TestInputScope::LiveInstanceCount() == 0`;
* `cleanup_report().store_instances_alive == 0`;
* `cleanup_report().advised_store_sinks_alive == 0`;
* `all_documents_disconnected && all_contexts_released`;
* `store_reference_count_after_shutdown == 0`.

`abandon_pending_query` additionally proves that a replacement query on a different
context, begun while the abandoned query's deferred grant is still queued, keeps its own
identity/generation and is not rewritten by the late callback.

## 9. Known limits and compatibility

1. **InputScope is not a security boundary.** An application may omit or lie about its
   metadata. `Allowed` is “no known blocking metadata”, never “safe content”.
2. **No one-time-code scope exists in this SDK revision.** `IS_ONE_TIME_CODE` is absent
   from `um/inputscope.h` 10.0.26100.0; EpoCanva neither invents it nor guesses OTP fields
   from text.
3. **Owner attribute failures are indistinguishable from absence on this build.** A
   store whose `RetrieveRequestedAttrs` returns `E_FAIL` surfaces the same `E_FAIL` from
   `GetAppProperty(...)->GetValue` as an attribute-free owner. The suite pins both as
   absent (`scope-owner-failure` prints the gap). A future msctf build that propagates a
   different HRESULT would classify it as `Unknown` automatically.
4. **`E_UNEXPECTED` from a cleared compartment** is mapped to `Unknown` by the reader, but
   a fresh `GetCompartment` recreates the compartment and reads the official no-value
   answer; the test pins the recreated path.
5. **No focus tracking.** `ITfThreadMgr::SetFocus` is not used (it returns `E_FAIL` in the
   headless test process); `AssociateFocus` and real focus changes are unverified. This
   phase deliberately does not implement focus or external cursor monitoring.
6. **No registration, key sink, candidate UI, or external application write.** A green
   suite is not an installed input method.
7. **The callback recheck is metadata-only.** It reads status, two compartments and
   InputScope with the granted cookie; it never reads document text. It cannot detect an
   application that lies, and it does not inspect the content being typed.
8. **Generation identity is coordinator-assigned.** `context_id` is host-assigned in the
   tests; the coordinator trusts the host's id and its own generation counter, and does
   not derive identity from COM pointer values.
9. **The engine-side `NeedsResync` after a clean refusal** is covered with the controlled
   backend (direct counters); the real-engine suite covers the same terminal through real
   TSF read-only/lock refusals and permission flips, not through an engine fault that the
   pinned rime build cannot be made to produce.
10. **No process-wide uncertain-text registry.** The `Uncertain` lockdown is keyed by
    context id inside one coordinator instance: re-binding the same context through that
    instance is refused, but a host that constructs a *new* coordinator could bypass it.
    The contract forbids using Close/recreation as a silent bypass; the coordinator cannot
    enforce a host decision to discard its own state. Recovery from an uncertain write is
    the host's explicit decision, not an automatic coordinator action.
11. **No performance claims.** The Debug timing failure above is unrelated to this change.

## 10. Source map

| File | Role |
| --- | --- |
| `platforms/windows/tsf_context/input_permission.{h,cc}` | pure facts → verdict classification; sensitive-set policy; findings |
| `platforms/windows/tsf_context/permission_query.{h,cc}` | real `PermissionSource`, read-only edit session, `ReadPermissionFactsWithCookie` for the callback-time recheck, diagnostics; `inputscope_guids.cc` defines the InputScope GUIDs once |
| `platforms/windows/tsf_edit/edit_session.{h,cc}` | `SetPermissionRecheck` and its invocation inside `DoEditSession`, before any write |
| `platforms/windows/tsf_coordinator/coordinator.{h,cc}` | binding, `PermissionPending`, submit/apply-time queries, divergence terminals, `permission_rechecks` counter |
| `platforms/windows/tsf_coordinator/engine_to_plan.{h,cc}` | the reordered pure observation→plan rules |
| `tests/windows/tsf_context/permission_tests.cc` | parser and real-read cases |
| `tests/windows/tsf_coordinator/input_permission_gate_tests.cc` | the 9 real-metadata coordinator cases |
| `tests/windows/tsf_edit/test_{host,text_store}.{h,cc}` | real TSF host; scope publication, `refuse_selection_reads`, `kEmptyValue` |
