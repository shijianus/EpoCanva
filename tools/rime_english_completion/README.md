# `tools/rime_english_completion/` — retired English integration test entry

This directory is empty of code on purpose. Nothing here is runnable.

## What was here

`main.cc` (the P2.1–P2.6 integration harness, one ~5,100 line translation unit) and
`CMakeLists.txt` (its project file and CTest registrations, prefix `english.*`).

`main.cc` was truncated on 2026-10-19 and was committed empty in snapshot `0c4bc62` so the
loss stayed visible in history rather than being quietly deleted. `CMakeLists.txt` was then
modified at 2026-10-19 16:40 — after the loss — and still registered
`english.pilot.spelling_suggestion_cost_a_b_on_pilot_data` (`--mode pilot-spelling`) for a
mode that never existed in any green run. A project file that registers a test ahead of its
implementation cannot be built, and the two files together could no longer say anything about
the current state of the suite.

Both were removed in `build: promote modular English integration test entrypoint`. Neither was
replaced in place.

## What to use instead

The rebuilt, modular harness lives in `tools/rime_english_recovery/` (one file per behaviour
group plus the shared harness), and its formal entry point is:

```
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/test_english_integration.ps1 `
  -EnginePrefix  build/librime-english/rime/install `
  -BaselinePrefix build/librime-baseline/rime/install `
  -BuildDir      build/english-recovery/test
```

Add `-PilotLexicon <absolute path to the generated word list>` to configure and run the pilot
cases as well. Without it no pilot case is registered, no generated data is required and no
manifest is read. See `docs/testing/rime-english-completion.md` for the whole contract.

## Where the old case names went

The old registrations are not lost and are not silently dropped: every one of them is listed
with its replacement and its status in the coverage matrix in
`docs/reports/integration-harness-recovery-status.md`. That table is the record of what was
recovered, what was never finished before the incident, and what remains to do — including
`pilot-spelling`, which was registered-but-unimplemented here and is still unimplemented.

`tests/english_completion/assert_exit.cmake` (the exit-code-plus-marker wrapper) and
`tests/fixtures/english_completion/` survive untouched and are still what the new project
runs against.
