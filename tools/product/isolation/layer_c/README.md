# W3-A normal-privilege layer C test tools

These are maintained test tools, not product code and not part of any product package.

## Preflight (no UAC, no registration, no input)

`run_preflight_3.ps1` runs the **same normal-privilege agent** three times with
`-PreflightOnly`. Each run:

- records the agent PID/parent, user, session, token integrity, window station,
  thread desktop, input desktop and foreground;
- requires zero pre-existing Notepad processes (never reuses or closes a user instance);
- creates a unique empty test file, launches Notepad with an absolute path, and records
  the raw `CreateProcessW` BOOL, the immediately-read Win32 error, any exception,
  `STARTUPINFO.cb`, `PROCESS_INFORMATION`, launcher/window PIDs and process creation times;
- records the window class/title, verifies the window process is new and its title contains
  the unique test file, checks UIA for the `Document`/`RichEditD2DPT` control, and closes
  only that owned window/process;
- sends no keyboard or mouse input.

Run from the tools directory or an immutable run copy:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File <tool-dir>\run_preflight_3.ps1 `
  -RepoRoot <repo> -TestRoot <repo>\build\w3a-c-local\test-documents `
  -EvidenceRoot <repo>\build\w3a-c-local\preflight -Runs 3
```

`stage_run_copy.ps1` copies the maintained tools to an immutable directory below
`build/w3a-c-local/run-<timestamp>` and writes `run-identity.json` (source git HEAD and
SHA-256 of every run copy). `build/` contains only run copies and evidence.

## Formal handshake (not executed by preflight)

The formal order is:

1. normal-integrity `c_layer_agent.ps1` starts and performs preflight;
2. it writes `preflight-ready.flag` with the session ID and waits;
3. only then `trigger.ps1 -Elevate -StageDir <frozen stage>` requests one UAC and starts
   `orchestrator.ps1`;
4. the orchestrator registers/activates and writes `registration-ready.flag` with the same
   session ID;
5. the same agent creates the formal Notepad, writes `test-window-ready.flag`, then
   `test-start.flag` immediately before the first key;
6. the orchestrator cleans up and writes `cleanup-complete.flag`.

Every flag is bound to one session ID and one absolute deadline. Sleep/wake, wall-clock
jumps, agent exit, cleanup-before-start, missing registration or a pre-existing Notepad stop
the round. No approval is reused. The elevated process performs registration/removal only;
it does not set the foreground or send input.
