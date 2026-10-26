# EpoCanva P3.8 -- coordinator verification program and its CTest cases.
#
# NOT A STANDALONE PROJECT. This directory is included by
# platforms/windows/engine_bridge/CMakeLists.txt after epocanva_tsf_coordinator exists.
#
# WHAT IS REUSED, NOT REBUILT: the P3.7 controlled TSF host (test_host/test_text_store/
# test_support from tests/windows/tsf_edit, compiled from their one set of sources) and the
# P3.5 executor scaffolding (test_support + controlled_backend from
# tests/windows/engine_executor, same). No second text store, no second key translator, no
# second controlled backend is written here.
#
# The stage-1 cases use the controlled backend and load no rime.dll; the stage-2 cases use
# the real English engine. Both run against the real TSF manager through the P3.7 host.
if(NOT TARGET epocanva_tsf_coordinator)
  message(FATAL_ERROR
    "tests/windows/tsf_coordinator must be included from platforms/windows/engine_bridge; "
    "it links epocanva_tsf_coordinator.")
endif()

set(_tsf_edit_tests_dir "${CMAKE_CURRENT_LIST_DIR}/../tsf_edit")
set(_executor_tests_dir "${CMAKE_CURRENT_LIST_DIR}/../engine_executor")
# P3.10: the external-edit and focus helpers are shared with the observer suite.
set(_observer_tests_dir "${CMAKE_CURRENT_LIST_DIR}/../tsf_context_observer")

add_executable(tsf_coordinator_tests
  main.cc
  coordinator_test_support.cc
  coordinator_test_support.h
  coordination_tests.cc
  coordination_tests.h
  input_permission_gate_tests.cc
  input_permission_gate_tests.h
  observer_binding_tests.cc
  observer_binding_tests.h
  real_engine_tests.cc
  real_engine_tests.h
  "${_observer_tests_dir}/observer_test_support.cc"
  "${_observer_tests_dir}/observer_test_support.h"
  "${_tsf_edit_tests_dir}/inputscope_test_guids.cc"
  "${_tsf_edit_tests_dir}/test_host.cc"
  "${_tsf_edit_tests_dir}/test_host.h"
  "${_tsf_edit_tests_dir}/test_text_store.cc"
  "${_tsf_edit_tests_dir}/test_text_store.h"
  "${_tsf_edit_tests_dir}/test_support.cc"
  "${_tsf_edit_tests_dir}/test_support.h"
  "${_executor_tests_dir}/test_support.cc"
  "${_executor_tests_dir}/test_support.h"
  "${_executor_tests_dir}/controlled_backend.cc"
  "${_executor_tests_dir}/controlled_backend.h")
target_compile_definitions(tsf_coordinator_tests PRIVATE
  _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_compile_options(tsf_coordinator_tests PRIVATE
  /W4 /permissive- /EHsc /Zc:__cplusplus /utf-8)
target_include_directories(tsf_coordinator_tests PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}"
  # The P3.7 host headers (test_host.h includes test_text_store.h; both live in that one
  # directory). Quote-includes inside the reused .cc files resolve to their own directory
  # first, so having both suites' directories on the path cannot redirect them.
  "${_tsf_edit_tests_dir}"
  "${_observer_tests_dir}"
  # tests/windows -- so "engine_executor/test_support.h" is unambiguous even though both
  # suites have a file named test_support.h.
  "${CMAKE_CURRENT_LIST_DIR}/..")
target_link_libraries(tsf_coordinator_tests PRIVATE
  epocanva_tsf_coordinator epocanva_engine_keyboard_adapters ole32 uuid user32)

enable_testing()

set(_coordinator_assert
  "${CMAKE_CURRENT_LIST_DIR}/../../english_completion/assert_exit.cmake")

function(epocanva_add_coordinator_case name case)
  add_test(
    NAME ${name}
    COMMAND ${CMAKE_COMMAND}
      -DEXE=$<TARGET_FILE:tsf_coordinator_tests>
      -DCASE=${case}
      -DEXPECTED_EXIT=0
      "-DMARKER=RESULT PASS"
      -DMARKER_POLICY=require
      "-DPASS_ARGS=--case;${case};${ARGN}"
      -DTIMEOUT_SECONDS=180
      -P ${_coordinator_assert})
endfunction()

# --- stage 1: coordination over the controlled seam (no rime.dll) ------------------------

# 1. The pure engine-observation -> edit-decision matrix, including the refused
#    commit+preedit shape and the unreadable-observation refusals.
epocanva_add_coordinator_case(
  windows.tsf_coordinator.conversion_pure conversion-pure)

# 2. Binding, generations, the Unknown/Denied gates, non-candidate keys, invalid arguments.
epocanva_add_coordinator_case(
  windows.tsf_coordinator.bind_and_gates bind-gates)

# 2b. P3.13 controlled target-status mapping (test-owned revalidator, no TSF notification).
epocanva_add_coordinator_case(
  windows.tsf_coordinator.target_status_mapping target-status-mapping)

# 3. Single flight: a second submit is Busy, exactly one dispatch, the slot frees.
epocanva_add_coordinator_case(
  windows.tsf_coordinator.busy_single_flight busy-single-flight)

# 4. h -> he -> hel through the real TSF store with real Begin/Update edit sessions.
epocanva_add_coordinator_case(
  windows.tsf_coordinator.composition_progress_controlled composition-progress-controlled)

# 5. Evaluate is pure: repeated evaluations submit nothing and touch nothing.
epocanva_add_coordinator_case(
  windows.tsf_coordinator.evaluate_pure evaluate-pure)

# 6. Invalidation while the engine result is still owed: the old binding retires, its result
#    and close are consumed once, a new binding is admitted, and input pauses as Retiring
#    until the retirement is terminal.
epocanva_add_coordinator_case(
  windows.tsf_coordinator.engine_pending_invalidate engine-pending-invalidate)

# 6b. QueueFull while retiring: the close is retried, not lost; the bounded ledger refuses
#     a third binding as Retiring instead of creating more clients.
epocanva_add_coordinator_case(
  windows.tsf_coordinator.retirement_queue_full retirement-queue-full)

# 6c. A live composition whose cleanup needs a legal lock: the new binding is admitted but
#     its input is Retiring until the old composition is finished; then it resumes.
epocanva_add_coordinator_case(
  windows.tsf_coordinator.retirement_compose_withheld retirement-compose-withheld)

# 6d. The application owner ends the test composition. Retirement must attribute the end to
#     that external operation: no Finished, no AlreadyOrdinary, no end call of ours. One
#     case terminates before cleanup starts, the other while the cleanup plan is pending.
epocanva_add_coordinator_case(
  windows.tsf_coordinator.retirement_compose_external_before
  retirement-compose-external-before)
epocanva_add_coordinator_case(
  windows.tsf_coordinator.retirement_compose_external_pending
  retirement-compose-external-pending)

# 6e. A cleanup that is refused by real permission metadata is Blocked, not Finished.
epocanva_add_coordinator_case(
  windows.tsf_coordinator.retirement_compose_permission_denied
  retirement-compose-permission-denied)

# 6f. An external edit changed the composition range. Cleanup is Blocked(RangeMismatch),
#     never deletes or rewrites the foreign text.
epocanva_add_coordinator_case(
  windows.tsf_coordinator.retirement_compose_range_mismatch
  retirement-compose-range-mismatch)

# 7. Invalidation while an async edit waits behind a deferred lock: the old callback
#    refuses, neither document is written, the new binding works.
epocanva_add_coordinator_case(
  windows.tsf_coordinator.edit_pending_invalidate edit-pending-invalidate)

# 8. Permission changed while the engine result was queued: the work is rejected at apply
#    time, no edit was requested, and later submits are refused at the gate.
epocanva_add_coordinator_case(
  windows.tsf_coordinator.permission_change_queued permission-change-queued)

# 9. Permission changed while an edit was pending: refused inside the real callback.
epocanva_add_coordinator_case(
  windows.tsf_coordinator.permission_change_edit_pending permission-change-edit-pending)

# 10. Read-only, refused write locks and a disconnected context are never Applied.
epocanva_add_coordinator_case(
  windows.tsf_coordinator.refusals_not_applied refusals-not-applied)

# 11. An engine-side fault becomes NeedsResync; input is refused, not replayed; an explicit
#     invalidation is the way out.
epocanva_add_coordinator_case(
  windows.tsf_coordinator.needs_resync_block needs-resync-block)

# 12. Close with pending work: terminal state, honest leftover-result accounting.
epocanva_add_coordinator_case(
  windows.tsf_coordinator.close_pending_work close-pending-work)

# 13. Pumping after a terminal state writes and consumes nothing.
epocanva_add_coordinator_case(
  windows.tsf_coordinator.pump_idempotent pump-idempotent)

# 14. The falsifiability control: one deliberately false claim, exit 14, no RESULT PASS.
add_test(
  NAME windows.tsf_coordinator.can_fail
  COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:tsf_coordinator_tests>
    -DCASE=can-fail
    -DEXPECTED_EXIT=14
    "-DMARKER=RESULT PASS"
    -DMARKER_POLICY=forbid
    "-DPASS_ARGS=--case;can-fail"
    -DTIMEOUT_SECONDS=120
    -P ${_coordinator_assert})

# --- P3.9: coordinator admission over REAL TSF metadata ---------------------------------
#
# The production PermissionSource reads GetStatus, the KEYBOARD_DISABLED/EMPTYCONTEXT
# compartments and GUID_PROP_INPUTSCOPE through real edit sessions on the P3.7 host.
# The engine is the controlled backend, so the "no engine call" checks are direct
# counters. No case hands the coordinator an Allowed value.

epocanva_add_coordinator_case(
  windows.tsf_coordinator.permission_real_allow permission-real-allow)
epocanva_add_coordinator_case(
  windows.tsf_coordinator.permission_real_deny permission-real-deny)
epocanva_add_coordinator_case(
  windows.tsf_coordinator.permission_real_unknown permission-real-unknown)
epocanva_add_coordinator_case(
  windows.tsf_coordinator.permission_real_async permission-real-async)
epocanva_add_coordinator_case(
  windows.tsf_coordinator.permission_real_flip_before_apply
  permission-real-flip-before-apply)
epocanva_add_coordinator_case(
  windows.tsf_coordinator.permission_real_engine_ran_refused
  permission-real-engine-ran-refused)
epocanva_add_coordinator_case(
  windows.tsf_coordinator.permission_real_generation permission-real-generation)
epocanva_add_coordinator_case(
  windows.tsf_coordinator.permission_real_close_pending
  permission-real-close-pending)
epocanva_add_coordinator_case(
  windows.tsf_coordinator.permission_real_balance permission-real-balance)

set_tests_properties(
  windows.tsf_coordinator.permission_real_allow
  windows.tsf_coordinator.permission_real_deny
  windows.tsf_coordinator.permission_real_unknown
  windows.tsf_coordinator.permission_real_async
  windows.tsf_coordinator.permission_real_flip_before_apply
  windows.tsf_coordinator.permission_real_engine_ran_refused
  windows.tsf_coordinator.permission_real_generation
  windows.tsf_coordinator.permission_real_close_pending
  windows.tsf_coordinator.permission_real_balance
  PROPERTIES TIMEOUT 240
             LABELS "windows;tsf;coordinator;permission;metadata;real")

# --- P3.10: coordinator driven by the real context observer ----------------------------
#
# The bindings in these cases come from ObservedBindingController, not from a direct
# Bind() call. External selection/text changes are real read/write edit sessions with a
# distinct real TfClientId. The engine is the controlled backend, so engine-call counts are
# direct; the permission source is the production real-metadata source.

epocanva_add_coordinator_case(
  windows.tsf_coordinator.observed_bind_and_type observed-bind-and-type)
epocanva_add_coordinator_case(
  windows.tsf_coordinator.observed_external_selection observed-external-selection)
epocanva_add_coordinator_case(
  windows.tsf_coordinator.observed_external_text observed-external-text)
epocanva_add_coordinator_case(
  windows.tsf_coordinator.observed_late_permission observed-late-permission)
epocanva_add_coordinator_case(
  windows.tsf_coordinator.observed_late_edit observed-late-edit)
epocanva_add_coordinator_case(
  windows.tsf_coordinator.observed_uncertain_swap observed-uncertain-swap)
epocanva_add_coordinator_case(
  windows.tsf_coordinator.observed_permission_requery observed-permission-requery)

# --- P3.13: live target revalidation and focus admission -------------------------------
#
# These cases use the production ContextObserver as the target authority. A real Win32
# thread-focus window is created by the test scaffolding; it is labelled as a revalidation,
# never as a TSF notification. The cases cover a target move before input, after the engine
# result, while an async edit waits, thread-focus loss, no document manager/no top context,
# a generation move, and the bounded-retirement rebind to the latest target without replay.

epocanva_add_coordinator_case(
  windows.tsf_coordinator.focus_guard_submit_target focus-guard-submit-target)
epocanva_add_coordinator_case(
  windows.tsf_coordinator.focus_guard_engine_target focus-guard-engine-target)
epocanva_add_coordinator_case(
  windows.tsf_coordinator.focus_guard_edit_target focus-guard-edit-target)
epocanva_add_coordinator_case(
  windows.tsf_coordinator.focus_guard_thread_lost focus-guard-thread-lost)
epocanva_add_coordinator_case(
  windows.tsf_coordinator.focus_guard_foreign_dm focus-guard-foreign-dm)
epocanva_add_coordinator_case(
  windows.tsf_coordinator.focus_guard_generation focus-guard-generation)
epocanva_add_coordinator_case(
  windows.tsf_coordinator.focus_guard_retire_rebind focus-guard-retire-rebind)

# P3.13-R2: path A (a real OnPushContext/OnPopContext notification invalidates first) and
# the strict-policy integration probe (a real Win32 window is present but never accepted).
epocanva_add_coordinator_case(
  windows.tsf_coordinator.focus_notification_before_guard
  focus-notification-before-guard)
epocanva_add_coordinator_case(
  windows.tsf_coordinator.focus_revalidate_before_notification
  focus-revalidate-before-notification)
epocanva_add_coordinator_case(
  windows.tsf_coordinator.focus_guard_strict_no_fallback focus-strict-no-fallback)

set_tests_properties(
  windows.tsf_coordinator.focus_guard_submit_target
  windows.tsf_coordinator.focus_guard_engine_target
  windows.tsf_coordinator.focus_guard_edit_target
  windows.tsf_coordinator.focus_guard_thread_lost
  windows.tsf_coordinator.focus_guard_foreign_dm
  windows.tsf_coordinator.focus_guard_generation
  windows.tsf_coordinator.focus_guard_retire_rebind
  windows.tsf_coordinator.focus_notification_before_guard
  windows.tsf_coordinator.focus_revalidate_before_notification
  windows.tsf_coordinator.focus_guard_strict_no_fallback
  PROPERTIES TIMEOUT 300
             LABELS "windows;tsf;coordinator;focus;target;revalidation")

set_tests_properties(
  windows.tsf_coordinator.observed_bind_and_type
  windows.tsf_coordinator.observed_external_selection
  windows.tsf_coordinator.observed_external_text
  windows.tsf_coordinator.observed_late_permission
  windows.tsf_coordinator.observed_late_edit
  windows.tsf_coordinator.observed_uncertain_swap
  windows.tsf_coordinator.observed_permission_requery
  PROPERTIES TIMEOUT 300
             LABELS "windows;tsf;coordinator;observer;context;generation")

# --- stage 2: the real English engine through the real TSF store ------------------------
#
# Each case gets its own recreated run root (PrepareRunRoot requires the leaf name
# engine-executor-run), the fixed English fixtures and the already-built rime.dll named by
# EPOCANVA_RIME_DLL. Nothing is rebuilt or fetched here.

set(_coordinator_run_base "${CMAKE_CURRENT_BINARY_DIR}/run")
set(_coordinator_engine_args
  "--rime-dll;${EPOCANVA_RIME_DLL}"
  "--fixture-dir;${EPOCANVA_ENGLISH_FIXTURE_DIR}"
  "--spelling-fixture-dir;${EPOCANVA_SPELLING_FIXTURE_DIR}")

function(epocanva_add_real_coordinator_case name case)
  add_test(
    NAME ${name}
    COMMAND ${CMAKE_COMMAND}
      -DEXE=$<TARGET_FILE:tsf_coordinator_tests>
      -DCASE=${case}
      -DEXPECTED_EXIT=0
      "-DMARKER=RESULT PASS"
      -DMARKER_POLICY=require
      "-DPASS_ARGS=--case;${case};${_coordinator_engine_args};--run-root;${_coordinator_run_base}/${case}/engine-executor-run"
      -DTIMEOUT_SECONDS=300
      -P ${_coordinator_assert})
endfunction()

# 1. h -> he -> hel updates the composition step by step.
epocanva_add_real_coordinator_case(
  windows.tsf_coordinator.real_compose_progress real-compose-progress)
# 2. Return leaves exactly one "hel".
epocanva_add_real_coordinator_case(
  windows.tsf_coordinator.real_return_commit real-return-commit)
# 3. Space leaves "hel " -- the word plus exactly one space.
epocanva_add_real_coordinator_case(
  windows.tsf_coordinator.real_space_commit real-space-commit)
# 4. Explicit "hello" acceptance then Space commits once, no duplicate word.
epocanva_add_real_coordinator_case(
  windows.tsf_coordinator.real_candidate_commit real-candidate-commit)
# 5. BackSpace shrinks the composition; Escape cancels only its own composition.
epocanva_add_real_coordinator_case(
  windows.tsf_coordinator.real_backspace_escape real-backspace-escape)
# 6. "hello world" between a prefix and a suffix; both survive verbatim.
epocanva_add_real_coordinator_case(
  windows.tsf_coordinator.real_surrounding_text real-surrounding-text)
# 7. Technical text (protected marks) stays verbatim end to end.
epocanva_add_real_coordinator_case(
  windows.tsf_coordinator.real_technical_text real-technical-text)
# 8. Unicode prefix and astral suffix are untouched by the English composition.
epocanva_add_real_coordinator_case(
  windows.tsf_coordinator.real_unicode_affixes real-unicode-affixes)
# 9. Repeated Evaluate submits nothing and touches nothing.
epocanva_add_real_coordinator_case(
  windows.tsf_coordinator.real_evaluate_pure real-evaluate-pure)
# 10. Busy: one in-flight work, the second submit refused, never executed twice.
epocanva_add_real_coordinator_case(
  windows.tsf_coordinator.real_busy_reject real-busy-reject)
# 11. Rebinding while the engine result is unconsumed: the old result writes nothing.
epocanva_add_real_coordinator_case(
  windows.tsf_coordinator.real_rebind_pending real-rebind-pending)
# 12. A generation change during an async edit: the old callback writes to neither doc.
epocanva_add_real_coordinator_case(
  windows.tsf_coordinator.real_async_generation real-async-generation)
# 13. Allowed -> Denied/Unknown at both re-verification points: no further edits.
epocanva_add_real_coordinator_case(
  windows.tsf_coordinator.real_permission_flip real-permission-flip)
# 14. Read-only, refused locks and a disconnected context are never Applied.
epocanva_add_real_coordinator_case(
  windows.tsf_coordinator.real_refusals_not_applied real-refusals-not-applied)
# 15. An unconfirmed write locks the coordinator down; same-context re-bind is refused.
epocanva_add_real_coordinator_case(
  windows.tsf_coordinator.real_uncertain_lockdown real-uncertain-lockdown)
# 16. Pumping after a terminal state writes and consumes nothing.
epocanva_add_real_coordinator_case(
  windows.tsf_coordinator.real_pump_idempotent real-pump-idempotent)
# 17. Close with pending engine work / a pending async edit: terminal, bounded, clean.
epocanva_add_real_coordinator_case(
  windows.tsf_coordinator.real_close_pending real-close-pending)
# 18. A destroyed context's queued callback path terminates boundedly, nothing written.
epocanva_add_real_coordinator_case(
  windows.tsf_coordinator.real_destroyed_context real-destroyed-context)

# 19. P3.10: h -> he -> hel through the observer-created binding and the real English
#     engine. One generation, no target loss, the real caret after every own edit.
epocanva_add_real_coordinator_case(
  windows.tsf_coordinator.observed_real_compose observed-real-compose)
# 20. P3.12: A -> B -> A through the observer with no runtime restart. Each ended binding
#     closes its engine client and consumes the close result; only the current client is
#     live.
epocanva_add_real_coordinator_case(
  windows.tsf_coordinator.observed_retire_roundtrip observed-retire-roundtrip)
# 21. P3.12: a live composition on A is finished (EndComposition without SetText) when the
#     binding retires; the displayed text stays, the real composition enumeration drops to
#     zero, and B's input resumes only after the cleanup is terminal.
epocanva_add_real_coordinator_case(
  windows.tsf_coordinator.observed_compose_retirement observed-compose-retirement)
# 21b. P3.13-R3: a composition that was committed normally before retirement is
#      AlreadyOrdinary, not Finished: this cleanup issued no end call.
epocanva_add_real_coordinator_case(
  windows.tsf_coordinator.observed_compose_already_ordinary
  observed-compose-already-ordinary)
# 22. P3.12: a read-only change blocks the composition cleanup explicitly; the displayed
#     text is preserved, no destructive cleanup runs, and the block stays visible.
epocanva_add_real_coordinator_case(
  windows.tsf_coordinator.observed_compose_readonly observed-compose-readonly)
# 23. P3.12: an uncertain text state blocks cleanup without another TSF call; the real
#     context latch stays held and the independent document remains usable.
epocanva_add_real_coordinator_case(
  windows.tsf_coordinator.observed_compose_uncertain observed-compose-uncertain)
# 24. P3.12: an external edit beside a live composition. Retirement must not delete or
#     rewrite the text; the cleanup is Finished/AlreadyOrdinary or explicitly Blocked.
epocanva_add_real_coordinator_case(
  windows.tsf_coordinator.observed_compose_external observed-compose-external)

set_tests_properties(
  windows.tsf_coordinator.real_compose_progress
  windows.tsf_coordinator.real_return_commit
  windows.tsf_coordinator.real_space_commit
  windows.tsf_coordinator.real_candidate_commit
  windows.tsf_coordinator.real_backspace_escape
  windows.tsf_coordinator.real_surrounding_text
  windows.tsf_coordinator.real_technical_text
  windows.tsf_coordinator.real_unicode_affixes
  windows.tsf_coordinator.real_evaluate_pure
  windows.tsf_coordinator.real_busy_reject
  windows.tsf_coordinator.real_rebind_pending
  windows.tsf_coordinator.real_async_generation
  windows.tsf_coordinator.real_permission_flip
  windows.tsf_coordinator.real_refusals_not_applied
  windows.tsf_coordinator.real_uncertain_lockdown
  windows.tsf_coordinator.real_pump_idempotent
  windows.tsf_coordinator.real_close_pending
  windows.tsf_coordinator.real_destroyed_context
  windows.tsf_coordinator.observed_real_compose
  windows.tsf_coordinator.observed_retire_roundtrip
  windows.tsf_coordinator.observed_compose_retirement
  windows.tsf_coordinator.observed_compose_already_ordinary
  windows.tsf_coordinator.observed_compose_readonly
  windows.tsf_coordinator.observed_compose_uncertain
  windows.tsf_coordinator.observed_compose_external
  PROPERTIES TIMEOUT 360
             LABELS "windows;tsf;coordinator;engine;english;real")

message(STATUS "  tsf_coordinator stage-2 cases: 24 (real English engine, real TSF store)")

set_tests_properties(
  windows.tsf_coordinator.conversion_pure
  windows.tsf_coordinator.bind_and_gates
  windows.tsf_coordinator.target_status_mapping
  windows.tsf_coordinator.busy_single_flight
  windows.tsf_coordinator.composition_progress_controlled
  windows.tsf_coordinator.evaluate_pure
  windows.tsf_coordinator.engine_pending_invalidate
  windows.tsf_coordinator.retirement_queue_full
  windows.tsf_coordinator.retirement_compose_withheld
  windows.tsf_coordinator.retirement_compose_external_before
  windows.tsf_coordinator.retirement_compose_external_pending
  windows.tsf_coordinator.retirement_compose_permission_denied
  windows.tsf_coordinator.retirement_compose_range_mismatch
  windows.tsf_coordinator.edit_pending_invalidate
  windows.tsf_coordinator.permission_change_queued
  windows.tsf_coordinator.permission_change_edit_pending
  windows.tsf_coordinator.refusals_not_applied
  windows.tsf_coordinator.needs_resync_block
  windows.tsf_coordinator.close_pending_work
  windows.tsf_coordinator.pump_idempotent
  windows.tsf_coordinator.can_fail
  PROPERTIES TIMEOUT 240
             LABELS "windows;tsf;coordinator;context;binding;generation")

message(STATUS "  tsf_coordinator stage-1 cases: 18 (17 behavioural + can_fail)")
message(STATUS "  tsf_coordinator permission cases: 9 (real TSF metadata, controlled engine)")
