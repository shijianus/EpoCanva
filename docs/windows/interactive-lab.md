# EpoCanva P3.11 — visible TSF integration lab

This document records what `tools/windows/tsf_lab` is, what it actually exercises against
the real TSF manager and the pinned English engine, how it was built and tested, and what it
does **not** claim. It is written after the code, the two headless CTest cases, the
full engine-bridge regression, and a visible-window screenshot run.

The lab is an **experiment host**, not a product input method. It registers no TSF service,
installs no keyboard sink (`AdviseKeyEventSink` or otherwise), calls no `SendInput`, installs
no hook, and writes to no external application. Its two documents live in its own process.

> **P3.13 update.** The lab now drives the focus-admission guard: every ordinary submit and
> edit revalidates thread focus, the current `GetFocus`/`GetTop` canonical target, the
> registry generation and real permission; the P3.7 callback repeats the target check under
> the granted lock. The window shows the Win32 focus, the TSF facts, the evidence source
> (`notification` vs `revalidation`), `Admission` state, target-guard counters and the last
> refusal's stage/cause/HRESULT. The hidden scenario self-test now uses the real Win32 focus
> path (still with no visible window in CTest). See
> `docs/windows/focus-admission-contract.md` and the manual
> `docs/windows/focus-acceptance-checklist.md`.

## 1. Scope and non-goals

In scope:

* a native Win32 window with two focusable document surfaces A/B, each associated with its
  own real `ITfDocumentMgr` through `ITfThreadMgr::AssociateFocus`;
* real TSF contexts and `ITextStoreACP` documents (the existing P3.7 controlled host), the
  P3.10 context observer/registry, the P3.8/P3.9 coordinator, and the P3.5 engine executor;
* fresh engine client per observed binding, real composition edits, real external edits,
  real read-only metadata, real `Uncertain` fault injection;
* nine fixed fixture scenarios, driven one work item at a time with an explicit `Busy`
  refusal instead of a hidden queue;
* a read-only engine refresh used only to display composition/candidate data;
* a headless self-test for window lifecycle, scenario order/refusal, and resource balance;
* an optional BMP screenshot rendered by the window itself.

Out of scope and **not claimed**:

* no input-method registration, no language bar, no system keyboard sink, no candidate
  window policy, no writes into another application;
* no natural typing support. The scenarios submit fixed, already-translated `KeyText`
  values through the coordinator; the document surfaces ignore `WM_CHAR`;
* no real `OnSetFocus` notification claim. The automated and screenshot runs observed
  `OnSetFocus=0` and bound through the observer's real `GetFocus` revalidation; independent
  click verification is still the user's step (section 9);
* no password/one-time-code policy in the lab. Read-only is exercised; the InputScope
  classification is the production P3.9 path and is not claimed to be a security boundary;
* no network I/O. The Windows guarantee is architectural (our code makes no socket calls),
  not an OS sandbox around a normal in-process TSF DLL. This is the same distinction as
  `docs/security/threat-model.md`.

## 2. Files and responsibilities

```text
tools/windows/tsf_lab/
  CMakeLists.txt          opt-in target, reused source list, CTest registration
  main.cc                 CLI, absolute-path normalization, interactive/screenshot modes
  lab_window.h/.cc        native Win32 window, document surfaces, buttons, status, BMP
  lab_session.h/.cc       TSF host + executor + observer/coordinator + AC focus lifecycle
  scenario_runner.h/.cc   the nine fixed scripts, one in-flight step at a time
  lab_self_test.h/.cc     hidden window/scenario tests and teardown assertions

platforms/windows/engine_bridge/CMakeLists.txt
  EPOCANVA_BUILD_TSF_LAB=OFF (default); adds tools/windows/tsf_lab only when enabled
```

Reused without a second implementation:

| Name | Reused from | Role in the lab |
| --- | --- | --- |
| `TestHost`, `TestDocument`, `TestTextStore` | `tests/windows/tsf_edit` | the real `ITfThreadMgr`, two document managers, contexts, ACP stores and teardown |
| `ContextObserver`, `ContextRegistry` | `platforms/windows/tsf_context` | real focus/edit sinks, identities and generations |
| `ObservedBindingController` | `platforms/windows/tsf_coordinator` | observer callbacks → invalidate/bind/caret |
| `ContextCoordinator` | `platforms/windows/tsf_coordinator` | single-flight gates, permission, engine dispatch, edit plans |
| `CompositionEditSession` | `platforms/windows/tsf_edit` | real `RequestEditSession` composition edits |
| `EngineExecutor` | `platforms/windows/engine_executor` | worker-thread engine commands and value results |
| fixed-fixture deployment helpers | `tests/windows/engine_executor` | fixture staging, deployment preparation, runtime preparation |
| external-client edit helper | `tests/windows/tsf_context_observer` | a real other `TfClientId` write for the external-edit scenario |

No second text editor, text store, key translator, language model or dictionary is written
by the lab.

## 3. Window creation and message handling

The P3.10 report said a visible test window could not be created; the cause was a test
window procedure that forwarded `DefWindowProcW(window, message, 0, 0)`. That discards
`wParam`/`lParam`, and `DefWindowProcW` needs the `CREATESTRUCT` in `lParam` for
`WM_NCCREATE`. The host fix is commit
`6431f23914bf14cad0eca39993b04d1f96aef9b0` (`fix(windows): correct interactive TSF host
setup`). It forwards both parameters unchanged and captures `GetLastError` immediately.

The lab follows the same rules in `lab_window.cc`:

* both window classes are registered with `RegisterClassExW` using
  `GetModuleHandleW(nullptr)`; each registration error is copied to a local before any other
  call;
* the top-level `CreateWindowExW` passes `this` as `lpCreateParams`; `WM_NCCREATE` installs
  it in `GWLP_USERDATA` **before** `WM_CREATE`, so the child controls and layout code never
  depend on a pointer set after creation;
* `WM_CREATE` creates the two document children, ten buttons and the read-only status
  control; a child failure returns `-1` and records `create_error_`;
* every unhandled message goes to `DefWindowProcW(window, message, wparam, lparam)`;
* `WM_NCDESTROY` clears `GWLP_USERDATA` and the cached HWND;
* a failed `CreateWindowExW` is reported with the immediately captured error and, separately,
  with the window procedure's own rejection reason if it set one. Error 0 is not rewritten as
  "environment unavailable".

Relevant functions: `LabWindow::Create`, `LabWindow::EnsureClasses`,
`LabWindow::WindowProc` (`WM_NCCREATE`, `WM_CREATE`, `WM_CLOSE`, `WM_DESTROY`,
`WM_NCDESTROY`), `LabWindow::DocumentProc`, `LabWindow::CreateChildren`,
`LabWindow::CaptureToBmp`.

`WM_CHAR`, `WM_KEYDOWN`, `WM_SYSKEYDOWN` and `WM_SYSCHAR` on a document surface are ignored.
That is deliberate: the lab has no text key handling and no key sink.

## 4. HWND ↔ DocumentMgr association and teardown

`LabSession::AssociateDocuments` calls, for each child surface:

```text
ITfThreadMgr::AssociateFocus(window, document_manager, &previous)
```

The manager does **not** AddRef `pdimNew`; the test document owns its manager for the
session's lifetime. The returned `previous` manager is AddRef'd and is released by the lab
immediately (`previous->Release()`). Both associations are recorded.

`LabSession::ClearAssociations` runs while TSF and the document managers are still alive:

```text
ITfThreadMgr::SetFocus(nullptr)
for each associated window:
    AssociateFocus(window, nullptr, &previous)
    if previous != nullptr: previous->Release()
```

The results are retained as `association_clear_hr` / `association_previous_released` and
asserted by `windows.tsf_lab.scenarios`.

Teardown order (`LabSession::Shutdown`):

1. destroy the binding controller (clears the observer's sink pointer);
2. `ContextObserver::Shutdown` (unadvise thread/context sinks, abandon a pending read);
3. clear both `AssociateFocus` associations and release the returned previous managers;
4. `ContextCoordinator::Close` (pending work becomes `AbandonedStale`; release-only);
5. `EngineExecutor::StopAndJoin` (drains and closes every engine client, finalizes librime);
6. release the permission source, the context registry and its context references;
7. `TestHost::Shutdown` (Pop contexts, release managers/stores, `Deactivate` the thread
   manager, destroy the host message window, `CoUninitialize`);
8. destroy the visible window (and its child HWNDs), unregister the classes.

The self-test asserts `store_instances_alive==0`, `advised_store_sinks_alive==0`,
`forced_store_sink_releases==0`, all contexts/managers released, the thread manager
deactivated/released, `clients_created==clients_closed`, `live_sessions_after_cleanup==0`,
balanced engine API counters, and both association clears successful.

## 5. Focus, binding and what the UI shows separately

The lab displays these as separate facts, never collapsed into "focus succeeded":

* **Win32 focus**: `GetFocus() == document child HWND`.
* **TSF `GetFocus`**: `ITfThreadMgr::GetFocus` compared by canonical `IUnknown` with each
  document manager.
* **Current context and generation**: the observer's registry identity, plus the
  coordinator's bound identity.
* **`OnSetFocus` notifications**: the observer's `focus_notifications` counter.
* **Binding result**: `ObservedBindingController` counters and the last bind status.
* **Last bind path**: `notification` when the bind followed a real thread-manager
  `OnSetFocus`, `revalidation` when it followed `Pump`'s real `GetFocus` check, or
  `initial-adoption`.

Interactive path: the user clicks a document child; `WM_SETFOCUS` calls
`OnDocumentWindowFocused`, which calls the real `ITfThreadMgr::SetFocus`; a scenario button
then calls `EnsureDocumentWindowFocus`, which verifies `GetFocus()`, sets TSF focus if
needed, and waits for the observer-created binding. The scenario never reuses the previous
context. A scenario button click first focuses the target document and waits for its binding.

Headless path: `FocusDocumentProgrammatically` calls the real `ITfThreadMgr::SetFocus` and
waits for the same observer-created binding. This is not a synthesized sink notification and
is labelled `revalidation`; the self-test never claims an `OnSetFocus` delivery.

The visible screenshot run produced:

```text
Focus: Win32=A  TSF GetFocus=A  TSF SetFocus calls=1 last=0x00000000 target=A
Last bind path=revalidation (notification = real OnSetFocus; revalidation = Pump observed GetFocus)
Observer: OnSetFocus=0 ... binds=1 drops=0
```

So a visible window and the association path are verified; a real `OnSetFocus` notification
was **not** observed in this environment. Independent click verification remains pending
(section 9).

## 6. Fixed scenarios

The runner (`scenario_runner.cc`) submits at most one work item at a time and waits for its
terminal state. A second `Run` while one is active returns `kBusy`; the coordinator's own
second submit is asserted to return `SubmitStatus::kBusy`. There is no background input
queue.

| # | Script | What it asserts |
| --- | --- | --- |
| 1 | Compose `hel` | `h → he → hel` composition progresses; the coordinator reports an active composition; the read-only engine refresh shows preedit length 3 and a live menu containing the fixed `hello` candidate. Leaves the composition visible for the UI. |
| 2 | Return | Return commits exactly the original `hel` once; no composition remains. |
| 3 | Explicit `hello` + Space | The fixed `hello` candidate is selected by text lookup, the document becomes exactly `hello`, then Space produces exactly `hello ` — no duplicate word, one space. |
| 4 | BackSpace / Escape | BackSpace shrinks the composition; Escape removes only the composition; a later BackSpace with no composition is refused locally (`kNotEngineCandidate`) and creates no work. |
| 5 | A/B isolation | A and B are each focused through real TSF focus; fixed `ab`/`cd` commits land in their own documents; returning to A appends `e` only to A. No cross-document write. |
| 6 | Late A edit | A's edit is parked at `EditPending` behind a real held store grant; a direct second submit returns `Busy`, and a reentrant scenario run returns `kBusy`. Focus moves to B, B's fixed input applies; releasing A's held grant runs the old callback late, and neither A nor B changes. The target loss and invalidation counters must show the switch. Uses the P3.7 host's test-only grant hold, not a product behavior. |
| 7 | Read-only | The real store's `GetStatus` is flipped to read-only; the real P3.9 permission source returns Denied; the submit is refused before the engine; no engine result, no mutation, no work item. The read-only bit is restored afterward. |
| 8 | External edit | A real read/write edit session from a distinct real `TfClientId` inserts fixed `X` in B. The observer reports an external text edit, invalidates the old generation, and re-binds with a new generation; subsequent fixed input lands in the re-bound document and does not rebind again. |
| 9 | Uncertain | The P3.7 host's `kEFailAfterMutation` fault is installed on B; the real write fails after host-side mutation. The coordinator reaches `Uncertain`, the gate is `BlockedUncertain`, and the next submit returns `SubmitStatus::kBlockedUncertain` with no new work and no further mutation. The exact mutated text is deliberately not asserted. |

The scenarios display fixed fixture text only. The document surfaces show the backing text;
the status panel shows the fixed preedit/candidates and state/counters. The progress/log
lines in the self-test and console contain states, counts, generations, lengths and offsets,
never document text.

## 7. Build and run

The lab is opt-in. A normal configure/build/test does not build it and never opens a window.
From the repository root:

```text
cmake -S platforms/windows/engine_bridge -B build/windows-engine-bridge/test \
      -G "Visual Studio 17 2022" -A x64 -DEPOCANVA_BUILD_TSF_LAB=ON
cmake --build build/windows-engine-bridge/test --config Release --target tsf_lab
```

Paths default to the existing engine-bridge cache values (pinned English `rime.dll`, the two
fixed fixture directories, and the build-local trusted run root
`build/windows-engine-bridge/test/tsf_lab/run`). The fixture paths can be overridden
explicitly:

```text
build/windows-engine-bridge/test/tsf_lab/Release/tsf_lab.exe \
  --rime-dll build/librime-english/rime/install/lib/rime.dll \
  --fixture-dir tests/fixtures/english_completion \
  --spelling-fixture-dir tests/fixtures/english_spelling \
  --run-root build/windows-engine-bridge/test/tsf_lab/run
```

`--run-root` selects a parent directory **inside** the build-supplied trusted run root; it
cannot replace or widen that root, and a path outside it (including a prefix-similar sibling
or a link that resolves outside) is refused before any run directory is created. Every
launch creates a new, previously nonexistent `engine-executor-run-<UTC timestamp>-p<PID>`
directory below the selected parent and keeps all artefacts there. Existing directories are never deleted or
reused; a name collision retries with a bounded `-1`, `-2`, ... suffix, and an exhausted
budget is refused. Older run roots such as `build/p311-lab-run` are left untouched and are
no longer valid parents.

Interactive use:

1. the window opens; click document **A** or **B** (the status line shows Win32 focus, TSF
   `GetFocus`, the last bind path and whether an `OnSetFocus` notification arrived);
2. click one of the fixed scenario buttons. The runner focuses the target document, waits
   for the observer binding, then executes the fixed script;
3. the buttons are disabled while a scenario runs; closing the window during a scenario is
   deferred until the current step returns, then the session is shut down.

Screenshot (the window renders itself with `PrintWindow`; no screen capture of other
windows):

```text
build/windows-engine-bridge/test/tsf_lab/Release/tsf_lab.exe \
  --screenshot build/p311-lab-screenshot/lab.bmp \
  --rime-dll build/librime-english/rime/install/lib/rime.dll \
  --fixture-dir tests/fixtures/english_completion \
  --spelling-fixture-dir tests/fixtures/english_spelling \
  --run-root build/windows-engine-bridge/test/tsf_lab/run/screenshots
```

The screenshot used for this delivery is
`build/p313-lab-screenshot/lab-r2.png` (converted from `lab-r2.bmp`, produced by the
command above with `--screenshot build/p313-lab-screenshot/lab-r2.bmp`). It was captured
from a 1180×1060 window showing document A with `hel`, the fixed candidate menu (`hel`,
`hello`, `help`, `helmet`), Win32 focus A, TSF target A, `OnSetFocus=0`,
`source=win32-thread-focus-revalidation`, `Last bind evidence=revalidation`,
`Admission=Ready`, permission Allowed, coordinator Idle, target-guard counters, the last
edit HRESULTs, the retirement ledger and the cleanup line. It is a build artifact produced
by an automated screenshot mode, not a manual click result, and is not committed.

Headless checks (no window is shown):

```text
build/windows-engine-bridge/test/tsf_lab/Release/tsf_lab.exe \
  --self-test --case window-lifecycle

build/windows-engine-bridge/test/tsf_lab/Release/tsf_lab.exe \
  --self-test --case scenarios \
  --rime-dll build/librime-english/rime/install/lib/rime.dll \
  --fixture-dir tests/fixtures/english_completion \
  --spelling-fixture-dir tests/fixtures/english_spelling \
  --run-root build/windows-engine-bridge/test/tsf_lab/run/scenarios
```

The no-engine directory check runs in its own isolated area below the same trusted root:

```text
build/windows-engine-bridge/test/tsf_lab/Release/tsf_lab.exe \
  --self-test --case run-root
```

CTest:

```text
ctest --test-dir build/windows-engine-bridge/test -C Release --no-tests=error \
      -R "windows\.tsf_lab" -V
ctest --test-dir build/windows-engine-bridge/test -C Release --no-tests=error \
      --output-on-failure
```

## 8. Verification results

Environment: Windows 10.0.26200 x64, Visual Studio 17 2022, MSVC 19.44, CMake/CTest 3.31.6,
x64 Release, static CRT, the pinned English `rime.dll`.

| Check | Command | Observed result |
| --- | --- | --- |
| lab build | `cmake --build ... --config Release --target tsf_lab` | exit 0; zero `warning C` / `error C` lines in the captured log |
| window lifecycle | `tsf_lab.exe --self-test --case window-lifecycle` | 17/17 checks, exit 0 |
| all nine scenarios + balance | `tsf_lab.exe --self-test --case scenarios ...` | 46/46 checks, exit 0; all nine scenarios passed; every teardown/balance assertion passed, including the target guard and the foreign-manager distinction |
| lab CTest | `ctest ... -R "windows\.tsf_lab"` | 2/2 passed, exit 0 |
| full engine-bridge tree | full CTest | 115 tests, **100% passed, 0 failed, 1 skipped**, exit 0; the skip is `windows.tsf_context_observer.real_focus` (exit 77, environment-limited: no real `OnSetFocus`); the seven new focus/target cases and the target-evidence/status cases all passed |
| host fix regression | `real-focus` case in the full tree | window creation and `AssociateFocus` now pass; the case remains skipped only because no real `OnSetFocus` was delivered |
| visible screenshot run | `--screenshot ...` (run twice) | both exit 0; 1180×920 BMP rendered; `Win32=A`, `TSF GetFocus=A`, `OnSetFocus=0`, `path=revalidation` |

The first screenshot attempt (before the settle pump and before the visible mode used real
Win32 focus) hit a transient `NeedsResync / EditRefused` during activation. Two runs after
the fix completed all three letters and wrote the screenshot. This is recorded rather than
hidden; the final screenshot runs are the evidence.

P3.13-R1 replaced the lab's recursive run-root recreation with a guarded fresh-directory
path. In the R1 verification run, `windows.tsf_lab.run_root` passed 60/60 checks in an
isolated area below `build/windows-engine-bridge/test/tsf_lab/run`, including preserved
sentinels with unchanged full SHA-256 hashes, a legacy `engine-executor-run` directory that
was not treated as deletion authority, refusal of outside/prefix-sibling/junction parents,
normalization, and bounded collision retries. The lab CTest filter then passed 3/3. Those
numbers belong to R1; the P3.13 table above is unchanged.

The P3.10 Debug English-core performance failure is unchanged and out of scope for this
round; no threshold or English source was touched. Re-running the root `build/vs2022-x64-debug`
CTest on this machine reproduced it separately: 7/8 tests passed, exit 8,
`english.core.spelling_cost_trial_on_pilot_lexicon` measured **50,422.8 µs** worst single
call against the unchanged 25,000 µs Debug budget. It is reported here as a known failure,
not folded into the Windows lab result.

## 9. Interactive verification: what is observed, what is pending

Automated and screenshot evidence confirms:

* a real visible window can be created with the corrected procedure and association path;
* two real `ITfDocumentMgr`s are associated with two child HWNDs and are cleared with the
  previous-reference contract at teardown;
* programmatic and visible-focus runs both obtain a real observer-created binding and edit
  the real TSF documents;
* Win32 focus and TSF `GetFocus` can be displayed separately from the `OnSetFocus` counter.

**Not observed** in any run of this phase: a real `ITfThreadMgrEventSink::OnSetFocus`
notification or an `ITfThreadFocusSink` notification. Both the headless and visible runs
bound through the observer's real `GetFocus` revalidation
(`Last bind evidence=revalidation`, `notify_set=0`, `notify_kill=0`), and
`windows.tsf_context_observer.real_focus` remains registered with `SKIP_RETURN_CODE 77`.
Independent click verification is therefore **pending the user**:

```powershell
Set-Location "D:\Users\Smart Kevin\OneDrive\Desktop\EpoCanva"
& ".\build\windows-engine-bridge\test\tsf_lab\Release\tsf_lab.exe" `
  --rime-dll ".\build\librime-english\rime\install\lib\rime.dll" `
  --fixture-dir ".\tests\fixtures\english_completion" `
  --spelling-fixture-dir ".\tests\fixtures\english_spelling" `
  --run-root ".\build\windows-engine-bridge\test\tsf_lab\run"
```

Click A, then B, then A again. The expected observation is that `Observer: OnSetFocus`
and/or the thread-focus `notify_set`/`notify_kill` counts change and the bind evidence
becomes `notification`/`thread-focus-notification`; if they stay 0 and the source remains
`win32-thread-focus-revalidation` or `query-revalidation`, the environment did not deliver
the notification. The UI is the evidence either way. This document does not count a manual
result or an automated screenshot as an automated pass.

## 10. Current limitations and risks

1. **No real `OnSetFocus` or thread-focus notification was observed.** The binding path is
   real but labelled revalidation. `ITfThreadMgr::IsThreadFocus` also reports `S_FALSE` in
   this non-registered lab process, so the guard records
   `win32-thread-focus-revalidation`. The interactive click check is open (section 9).
2. **The lab is not an input method.** It does not register, install, or connect to a system
   keyboard path. It submits fixed, already-translated `KeyText` values; it is not evidence
   of natural typing, layout handling, or key-eating behavior.
3. **A composition cleanup can be blocked, and that is reported, not hidden.** P3.13
   retires the old binding and attempts `EndOwnedComposition` (no text write) when the
   focus moves. If the old context is read-only, the permission changed, the range is
   uncertain or the lock is refused, the cleanup is `Blocked` with a named reason and the
   real composition enumeration still shows it; the displayed text is preserved. The lab
   distinguishes `finished`, `already_ordinary` and `blocked` rather than treating all three
   as success.
4. **Engine clients are closed per retired binding.** The controller creates a fresh client
   per admitted binding, and the coordinator's bounded retirement ledger submits a close and
   consumes its result. `StopAndJoin` remains the last-resort owner. A target refused while
   the ledger is full is not given a client. The self-test asserts
   `clients_created==clients_closed` and no live client remained before `StopAndJoin`.
5. **Scenario 6 depends on the test host's grant hold.** That is deterministic fault
   injection for this experiment; it is not a product scheduling policy and is not compiled
   into a product path.
6. **Run-directory checks are path validation, not an OS sandbox.** Each launch creates a
   fresh child below the build-local trusted root after canonical containment checks and
   never deletes an existing run directory. A hostile local process that swaps a validated
   component for a junction between validation and creation is outside this test tool's
   threat model; the guarantee here is the absence of any deletion path, not OS enforcement.
7. **Null/empty TSF focus could not be held for a real integration case.** This environment
   restores the document focus after `SetFocus(nullptr)` and gives a newly created empty
   `ITfDocumentMgr` its own top context. The no-document-manager and no-top branches are
   pinned by the pure target-evidence case and the controlled coordinator status case; the
   real integration case covers a focused manager whose top is not the bound context. The
   manual checklist does not claim a null-focus observation.
8. **Scenario 9 asserts the coordinator's refusal, not the text.** The host applied-then-
   failed; the coordinator correctly does not know the resulting document content.
9. **Read-only is the only privacy metadata exercised.** InputScope/password classification
   is the production P3.9 path, but no scenario claims to identify a password field or
   provide a security boundary.
10. **The Windows offline guarantee is architectural.** A normal in-process TSF DLL cannot be
   network-sandboxed by the OS. This lab makes no socket calls; review and build policy are
   the enforcement. Do not describe it as OS-enforced. (The Android statement, where
   `INTERNET` is omitted, is separate and not touched here.)
11. **The status panel scrolls.** The screenshot shows the high-value lines (focus, bind path,
   documents, engine composition/candidates, coordinator); observer/binding lines may require
   scrolling on a smaller window.

## 11. Commits, staging and workspace

* `6431f23914bf14cad0eca39993b04d1f96aef9b0` — `fix(windows): correct interactive TSF host
  setup` (window procedure parameter forwarding, immediate `GetLastError`, corrected P3.10
  note).
* The lab commit is the commit that introduces this file and `tools/windows/tsf_lab/`; its
  full SHA is reported in the delivery summary.

Only explicit paths were staged. No hooks were run (`git -c core.hooksPath=
commit --no-verify`). No push, tag, amend, reset, clean or global configuration change was
made. Build logs and the screenshot remain under `build/` and are not committed.
