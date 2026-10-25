# EpoCanva P3.9 -- real input-permission verification program and its CTest cases.
#
# NOT A STANDALONE PROJECT. This directory is included by
# platforms/windows/engine_bridge after epocanva_tsf_context exists.
#
# WHAT IS REUSED, NOT REBUILT: the P3.7 controlled TSF host (test_host/test_text_store
# from tests/windows/tsf_edit, compiled from their one set of sources). No engine target
# is linked and no rime.dll is loaded: the permission module is engine-free, and these
# cases exercise the real msctf.dll metadata paths (GetStatus, context compartments,
# the GUID_PROP_INPUTSCOPE application property through a real read-only edit session).
if(NOT TARGET epocanva_tsf_context)
  message(FATAL_ERROR
    "tests/windows/tsf_context must be included from platforms/windows/engine_bridge; "
    "it links epocanva_tsf_context.")
endif()

set(_tsf_edit_tests_dir "${CMAKE_CURRENT_LIST_DIR}/../tsf_edit")

add_executable(tsf_context_tests
  main.cc
  permission_tests.cc
  permission_tests.h
  "${_tsf_edit_tests_dir}/inputscope_test_guids.cc"
  "${_tsf_edit_tests_dir}/test_host.cc"
  "${_tsf_edit_tests_dir}/test_host.h"
  "${_tsf_edit_tests_dir}/test_text_store.cc"
  "${_tsf_edit_tests_dir}/test_text_store.h")
target_compile_definitions(tsf_context_tests PRIVATE
  _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_compile_options(tsf_context_tests PRIVATE
  /W4 /permissive- /EHsc /Zc:__cplusplus /utf-8)
target_include_directories(tsf_context_tests PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}"
  "${_tsf_edit_tests_dir}")
target_link_libraries(tsf_context_tests PRIVATE
  epocanva_tsf_context ole32 uuid user32)

enable_testing()

set(_context_assert
  "${CMAKE_CURRENT_LIST_DIR}/../../english_completion/assert_exit.cmake")

function(epocanva_add_context_case name case)
  add_test(
    NAME ${name}
    COMMAND ${CMAKE_COMMAND}
      -DEXE=$<TARGET_FILE:tsf_context_tests>
      -DCASE=${case}
      -DEXPECTED_EXIT=0
      "-DMARKER=RESULT PASS"
      -DMARKER_POLICY=require
      "-DPASS_ARGS=--case;${case}"
      -DTIMEOUT_SECONDS=180
      -P ${_context_assert})
endfunction()

# 1. The pure classification matrix, no TSF involved.
epocanva_add_context_case(
  windows.tsf_context.pure_classification pure-classification)

# 2. Empirical probe of the real msctf metadata path: raw HRESULTs and VARIANT shapes
#    this machine actually produces, printed as reproducible evidence.
epocanva_add_context_case(
  windows.tsf_context.real_path_probe real-path-probe)

# 3. A plain editable context resolves Allowed through every real axis.
epocanva_add_context_case(
  windows.tsf_context.plain_editable_allowed plain-editable-allowed)

# 4. Read-only resolves Denied with the ReadOnlyContext finding.
epocanva_add_context_case(
  windows.tsf_context.read_only_denied read-only-denied)

# 5. KEYBOARD_DISABLED: several nonzero DWORD shapes (1, 2, LONG_MIN) all deny; zero
#    and the no-value default do not.
epocanva_add_context_case(
  windows.tsf_context.keyboard_disabled_values keyboard-disabled-values)

# 6. EMPTYCONTEXT nonzero denies.
epocanva_add_context_case(
  windows.tsf_context.empty_context_denied empty-context-denied)

# 7. Sensitive InputScopes (IS_PASSWORD and the pin/password set) deny, alone and in a
#    list, in any position; ordinary scopes do not.
epocanva_add_context_case(
  windows.tsf_context.sensitive_scopes_denied sensitive-scopes-denied)

# 8. Absent scope metadata, a normal empty value, a failing selection read, a wrong-typed
#    payload and the owner-side E_FAIL ambiguity.
epocanva_add_context_case(
  windows.tsf_context.scope_absent_wrongtype_fail scope-absent-wrongtype-fail)

# 9. A cleared compartment: the next GetCompartment recreates it (the documented
#    first-call-creates rule); the no-value answer is asserted with its grounding.
epocanva_add_context_case(
  windows.tsf_context.cleared_compartment_recreated cleared-compartment-recreated)

# 10. The same metadata queried repeatedly yields the same decision.
epocanva_add_context_case(
  windows.tsf_context.repeated_queries_consistent repeated-queries-consistent)

# 11. A deferred read session stays Pending and completes only after the host pumps.
epocanva_add_context_case(
  windows.tsf_context.async_query_pending async-query-pending)

# 12. Abandon ends a pending query immediately; a late callback publishes nothing.
epocanva_add_context_case(
  windows.tsf_context.abandon_pending_query abandon-pending-query)

# 13. Reference balance across query cycles and a clean host teardown.
epocanva_add_context_case(
  windows.tsf_context.resource_balance resource-balance)

# 14. The falsifiability control: one deliberately false claim, exit 14.
add_test(
  NAME windows.tsf_context.can_fail
  COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:tsf_context_tests>
    -DCASE=can-fail
    -DEXPECTED_EXIT=14
    "-DMARKER=RESULT PASS"
    -DMARKER_POLICY=forbid
    "-DPASS_ARGS=--case;can-fail"
    -DTIMEOUT_SECONDS=120
    -P ${_context_assert})

set_tests_properties(
  windows.tsf_context.pure_classification
  windows.tsf_context.real_path_probe
  windows.tsf_context.plain_editable_allowed
  windows.tsf_context.read_only_denied
  windows.tsf_context.keyboard_disabled_values
  windows.tsf_context.empty_context_denied
  windows.tsf_context.sensitive_scopes_denied
  windows.tsf_context.scope_absent_wrongtype_fail
  windows.tsf_context.cleared_compartment_recreated
  windows.tsf_context.repeated_queries_consistent
  windows.tsf_context.async_query_pending
  windows.tsf_context.abandon_pending_query
  windows.tsf_context.resource_balance
  windows.tsf_context.can_fail
  PROPERTIES TIMEOUT 180
             LABELS "windows;tsf;context;permission;real")

message(STATUS "  tsf_context cases: 14 (13 behavioural + can_fail)")
