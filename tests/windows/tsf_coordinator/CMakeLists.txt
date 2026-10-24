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

add_executable(tsf_coordinator_tests
  main.cc
  coordinator_test_support.cc
  coordinator_test_support.h
  coordination_tests.cc
  coordination_tests.h
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

# 3. Single flight: a second submit is Busy, exactly one dispatch, the slot frees.
epocanva_add_coordinator_case(
  windows.tsf_coordinator.busy_single_flight busy-single-flight)

# 4. h -> he -> hel through the real TSF store with real Begin/Update edit sessions.
epocanva_add_coordinator_case(
  windows.tsf_coordinator.composition_progress_controlled composition-progress-controlled)

# 5. Evaluate is pure: repeated evaluations submit nothing and touch nothing.
epocanva_add_coordinator_case(
  windows.tsf_coordinator.evaluate_pure evaluate-pure)

# 6. Invalidation while the engine result is still owed: discard, no write to any document,
#    Busy for a new binding until the discard completes.
epocanva_add_coordinator_case(
  windows.tsf_coordinator.engine_pending_invalidate engine-pending-invalidate)

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

set_tests_properties(
  windows.tsf_coordinator.conversion_pure
  windows.tsf_coordinator.bind_and_gates
  windows.tsf_coordinator.busy_single_flight
  windows.tsf_coordinator.composition_progress_controlled
  windows.tsf_coordinator.evaluate_pure
  windows.tsf_coordinator.engine_pending_invalidate
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

message(STATUS "  tsf_coordinator stage-1 cases: 14 (13 behavioural + can_fail)")
