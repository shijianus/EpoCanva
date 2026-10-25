# P3.13 desktop focus acceptance checklist (manual)

Status: **Pending Manual Verification**. No automated run, screenshot or scripted capture in
this repository counts as a manual click result. Record what you actually observe; if a step
cannot be completed in this environment, mark it `Blocked` or `Pending` rather than passing
it.

## 0. Build and launch

From the repository root in PowerShell:

```powershell
Set-Location "D:\Users\Smart Kevin\OneDrive\Desktop\EpoCanva"

& ".\build\windows-engine-bridge\test\tsf_lab\Release\tsf_lab.exe" `
  --rime-dll ".\build\librime-english\rime\install\lib\rime.dll" `
  --fixture-dir ".\tests\fixtures\english_completion" `
  --spelling-fixture-dir ".\tests\fixtures\english_spelling" `
  --run-root ".\build\windows-engine-bridge\test\tsf_lab\run"
```

Each launch creates a new retained run directory below this build-local parent; existing
run directories are never deleted. The console prints the exact new path.

The window title is `EpoCanva P3.13 - visible TSF integration lab`. Click document **A** or
**B** in the upper area to give it real Win32 focus; the status panel shows the focus facts.
Typed keys into the window are ignored; use the fixed scenario buttons.

The executable is not an input method: it registers no TSF service, installs no keyboard
sink, calls no `SendInput`, and writes to no other application.

## 1. Compose `hel` on A

1. Click document **A**.
2. Click **1. Compose hel**.
3. Observe:
   - `Focus: Win32=A thread_owned=1 ... TSF: GetFocus=A`;
   - `Document A ... comps=1` and the surface shows `hel`;
   - `Admission: Ready`, `permission=Allowed`, `phase=Idle`;
   - `Target guard ... refusals=0`.
4. Record `Last bind evidence` (`notification` or `revalidation`) and the
   `Thread focus ... source=` value. Do not report `revalidation` as an `OnSetFocus`
   notification.

Result: [ ] Pass  [ ] Fail  [ ] Blocked — notes:

## 2. Switch to B while A has a composition

1. Click document **B**.
2. Wait for the status to settle (`phase=Idle`, `retiring=0/2`).
3. Observe:
   - document A still shows `hel` and `comps=0` (the composition was finished, text kept);
   - `Document A ... mutations=3` and no extra text;
   - `Admission: Ready` on B after retirement, or `Retiring` while the cleanup is in
     progress;
   - `Retirement ... cleanup=Finished` or a named `Blocked` reason, never a silent success.
4. If the cleanup is `Blocked`, record `last_cleanup_block` exactly.

Result: [ ] Pass  [ ] Fail  [ ] Blocked — notes:

## 3. Return to A and type fresh input

1. Click document **A**.
2. Click **1. Compose hel** again (or **3. hello + Space** for a commit).
3. Observe:
   - the new composition starts at the current caret and does not contain an old word;
   - `Target guard ... rejected=0` (or a refusal reason if the environment refused);
   - `Last refusal: none` if admitted.
4. Record the actual document text/length; do not paste user text into the report.

Result: [ ] Pass  [ ] Fail  [ ] Blocked — notes:

## 4. Rapid switching and retirement recovery

1. Click A, B, A, B quickly several times without waiting.
2. Let the lab settle (the status ticks about once per second).
3. Observe:
   - `Retiring` may appear while records are live; `Admission` then returns to `Ready`;
   - `Binding ... rebind_pending` returns to `0` and `rebind_requests` increases;
   - the restored binding is the last document you clicked, not an earlier one;
   - no previously refused input appears in either document;
   - `close requests/results` match and `retiring=0/2` at rest.
4. Optionally repeat with a scenario button on the final target and confirm only its fixed
   text appears.

Result: [ ] Pass  [ ] Fail  [ ] Blocked — notes:

## 5. Leave the lab and return

1. Click another application (for example the PowerShell window).
2. Return to the lab window.
3. Observe:
   - while away, no new text or composition appears in A or B;
   - on return, `Focus`/`TSF: GetFocus` are consistent, and `Admission` is `Ready` or
     rebuilds through a fresh binding;
   - if the environment delivered `OnKillThreadFocus`/`OnSetThreadFocus`, the notification
     counts change and the evidence source says so; otherwise the source remains a
     revalidation and that is recorded as such.
4. Record whether `notify_set`/`notify_kill` changed. Do not claim notification delivery
   without seeing the counter change.

Result: [ ] Pass  [ ] Fail  [ ] Blocked — notes:

## 6. Read-only and Uncertain refusals

1. Click document **A** (or the document with text).
2. Click **7. Read-only**.
3. Observe a refusal before the engine:
   - `Last refusal: submit=PermissionDenied stage=permission ...`;
   - `permission=Denied` or the scenario's recorded refusal;
   - no document mutation and no new work item.
4. Click **9. Uncertain** on a document.
5. Observe:
   - `last terminal ... outcome=Uncertain`, `gate=BlockedUncertain`;
   - the next submit is `BlockedUncertain` with no mutation;
   - `Target guard`/`Last refusal` distinguish `uncertain` from `focus`, `permission`,
     `target-context`, `queue` and `engine-not-ready`.

Result: [ ] Pass  [ ] Fail  [ ] Blocked — notes:

## 7. Normal exit and cleanup

1. Click the window close button after a scenario settles (do not close during a running
   scenario).
2. The console prints a text-free line beginning `[lab] shutdown cleanup:`. Record it.
3. Check:
   - `stop=Ok`, `drain=complete/ok`;
   - `live_clients=0`, `created/closed` equal, `slots=0/16`;
   - `retiring=0/2`, `cleanup=Finished` or `Blocked/<reason>` with `finished`/`blocked`
     counts;
   - `stores_alive=0`, `advised_sinks_alive=0`, `thread_mgr_deactivated=1`,
     `released=1`;
   - `foreign_dms` and `still_enumerated` are reported. A real focused window may leave one
     TSF-owned manager; it is listed separately and is not a test-document leak.
4. If the window close is deferred because a scenario is running, wait for the scenario to
   return and then close.

Result: [ ] Pass  [ ] Fail  [ ] Blocked — notes:

## 8. Report template

```text
Build/commit: <full SHA>
Launch command: <exact command used>
Steps 1-7: <Pass/Fail/Blocked per step>
Last bind evidence on first A click: <notification|revalidation|initial-adoption>
Thread-focus source: <notification|query-revalidation|win32-thread-focus-revalidation|none>
OnSetThreadFocus/OnKillThreadFocus counts: <set>/<kill>
Shutdown cleanup line: <copy the [lab] shutdown cleanup line>
Any refusal lines: <copy Last refusal text, no user text>
Pending/blocked items: <list>
```

An automated capture of this window is evidence of rendering only. It is not a manual click
result and must not be entered as one.
