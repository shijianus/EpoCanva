# EpoCanva P3.10 -- context-observer verification program and its CTest cases.
#
# NOT A STANDALONE PROJECT. This directory is included by
# platforms/windows/engine_bridge after epocanva_tsf_context exists.
#
# WHAT IS REUSED, NOT REBUILT: the P3.7 controlled TSF host (test_host/test_text_store
# from tests/windows/tsf_edit, compiled from their one set of sources). No engine target is
# linked and no rime.dll is loaded: the observer layer is engine-free. The cases drive the
# REAL msctf.dll sinks and the REAL selection path; the only external edits are real
# read/write edit sessions issued with a distinct real TfClientId.
#
# real-focus is registered with SKIP_RETURN_CODE 77: if this environment cannot deliver an
# OnSetFocus notification (no OS input focus), the case prints RESULT ENV-LIMITED and CTest
# reports it as SKIPPED, never as passed. The interactive mode is not a CTest case:
#   tsf_context_observer_tests.exe --interactive-focus
if(NOT TARGET epocanva_tsf_context)
  message(FATAL_ERROR
    "tests/windows/tsf_context_observer must be included from "
    "platforms/windows/engine_bridge; it links epocanva_tsf_context.")
endif()

set(_observer_tsf_edit_tests_dir "${CMAKE_CURRENT_LIST_DIR}/../tsf_edit")

add_executable(tsf_context_observer_tests
  main.cc
  observer_test_support.cc
  observer_test_support.h
  observer_tests.cc
  observer_tests.h
  "${_observer_tsf_edit_tests_dir}/inputscope_test_guids.cc"
  "${_observer_tsf_edit_tests_dir}/test_host.cc"
  "${_observer_tsf_edit_tests_dir}/test_host.h"
  "${_observer_tsf_edit_tests_dir}/test_text_store.cc"
  "${_observer_tsf_edit_tests_dir}/test_text_store.h")
target_compile_definitions(tsf_context_observer_tests PRIVATE
  _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_compile_options(tsf_context_observer_tests PRIVATE
  /W4 /permissive- /EHsc /Zc:__cplusplus /utf-8)
target_include_directories(tsf_context_observer_tests PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}"
  "${_observer_tsf_edit_tests_dir}")
target_link_libraries(tsf_context_observer_tests PRIVATE
  epocanva_tsf_context ole32 uuid user32)

enable_testing()

set(_observer_assert
  "${CMAKE_CURRENT_LIST_DIR}/../../english_completion/assert_exit.cmake")

function(epocanva_add_observer_case name case)
  add_test(
    NAME ${name}
    COMMAND ${CMAKE_COMMAND}
      -DEXE=$<TARGET_FILE:tsf_context_observer_tests>
      -DCASE=${case}
      -DEXPECTED_EXIT=0
      "-DMARKER=RESULT PASS"
      -DMARKER_POLICY=require
      "-DPASS_ARGS=--case;${case}"
      -DTIMEOUT_SECONDS=180
      -P ${_observer_assert})
endfunction()

# 1. Canonical-IUnknown identity, monotonic ids, generations, the bounded latch, capacity.
epocanva_add_observer_case(
  windows.tsf_context_observer.registry_identity registry-identity)

# 2. The already-focused top context is adopted and bound from a real selection read.
epocanva_add_observer_case(
  windows.tsf_context_observer.initial_binding initial-binding)

# 3. OnPushContext/OnPopContext change the top context; the re-bind has a new generation.
epocanva_add_observer_case(
  windows.tsf_context_observer.push_pop_top push-pop-top)

# 4. An external selection move drops the old generation; Pump reads the new position.
epocanva_add_observer_case(
  windows.tsf_context_observer.external_selection external-selection)

# 5. An external text edit drops the old engine binding; the caret follows the real text.
epocanva_add_observer_case(
  windows.tsf_context_observer.external_text external-text)

# 6. A real non-collapsed selection closes admission; a later collapse reopens it.
epocanva_add_observer_case(
  windows.tsf_context_observer.noncollapsed_selection noncollapsed-selection)

# 7. OnUninitDocumentMgr: the focused manager disappears and no target remains.
epocanva_add_observer_case(
  windows.tsf_context_observer.document_uninit document-uninit)

# 8. Our own client's write (InWriteSession) does not invalidate; another client's does.
epocanva_add_observer_case(
  windows.tsf_context_observer.own_edit own-edit)

# 9. Real OnSetFocus delivery. SKIPPED (exit 77) when the environment cannot produce it.
add_test(
  NAME windows.tsf_context_observer.real_focus
  COMMAND $<TARGET_FILE:tsf_context_observer_tests> --case real-focus)
set_tests_properties(
  windows.tsf_context_observer.real_focus
  PROPERTIES TIMEOUT 180
             SKIP_RETURN_CODE 77
             LABELS "windows;tsf;context;observer;focus;real;environment-limited")

# 9. Shutdown with a deferred real read: the late callback runs on orphaned state.
epocanva_add_observer_case(
  windows.tsf_context_observer.shutdown_late_read shutdown-late-read)

# 10. Repeated observers: every COM reference, sink advice and store is balanced.
epocanva_add_observer_case(
  windows.tsf_context_observer.resource_balance resource-balance)

# 10b. P3.13 target-evidence decision table (pure facts; no TSF notification is synthesized).
epocanva_add_observer_case(
  windows.tsf_context_observer.target_evidence target-evidence)

# 11. The falsifiability control: one deliberately false claim, exit 14, no RESULT PASS.
add_test(
  NAME windows.tsf_context_observer.can_fail
  COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:tsf_context_observer_tests>
    -DCASE=can-fail
    -DEXPECTED_EXIT=14
    "-DMARKER=RESULT PASS"
    -DMARKER_POLICY=forbid
    "-DPASS_ARGS=--case;can-fail"
    -DTIMEOUT_SECONDS=120
    -P ${_observer_assert})

set_tests_properties(
  windows.tsf_context_observer.registry_identity
  windows.tsf_context_observer.initial_binding
  windows.tsf_context_observer.push_pop_top
  windows.tsf_context_observer.external_selection
  windows.tsf_context_observer.external_text
  windows.tsf_context_observer.noncollapsed_selection
  windows.tsf_context_observer.document_uninit
  windows.tsf_context_observer.own_edit
  windows.tsf_context_observer.shutdown_late_read
  windows.tsf_context_observer.resource_balance
  windows.tsf_context_observer.target_evidence
  windows.tsf_context_observer.can_fail
  PROPERTIES TIMEOUT 180
             LABELS "windows;tsf;context;observer;identity;selection;real")

message(STATUS
  "  tsf_context_observer cases: 12 (11 behavioural + can_fail; real_focus skippable)")
