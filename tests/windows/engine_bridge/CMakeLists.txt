# EpoCanva P3.4 -- engine bridge verification program and its CTest cases.
#
# NOT A STANDALONE PROJECT. This directory is included by
# platforms/windows/engine_bridge/CMakeLists.txt. It links the same static library whose
# runtime and session it judges; the DLL under test is reached at run time through the absolute
# path passed on the command line, never through the linker.
#
# Judging uses the repository's existing wrapper tests/english_completion/assert_exit.cmake:
# exact integer exit code plus a marker policy. No text from a live user is printed; every
# string an assertion mentions comes from the fixed fixtures under tests/fixtures/.

if(NOT TARGET epocanva_engine_bridge)
  message(FATAL_ERROR
    "tests/windows/engine_bridge must be included from platforms/windows/engine_bridge (the "
    "top-level project that builds epocanva_engine_bridge); it is not configurable alone.")
endif()

add_executable(engine_bridge_tests main.cc controller_tests.cc test_support.h)
target_compile_definitions(engine_bridge_tests PRIVATE
  _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_compile_options(engine_bridge_tests PRIVATE
  /W4 /permissive- /EHsc /Zc:__cplusplus /utf-8)
target_link_libraries(engine_bridge_tests PRIVATE
  epocanva_engine_bridge epocanva_engine_keyboard_adapters)

enable_testing()

set(_engine_assert "${CMAKE_CURRENT_LIST_DIR}/../../english_completion/assert_exit.cmake")
set(_engine_run_base "${CMAKE_CURRENT_BINARY_DIR}/run")

set(_engine_common_args
  "--rime-dll;${EPOCANVA_RIME_DLL}"
  "--fixture-dir;${EPOCANVA_ENGLISH_FIXTURE_DIR}"
  "--spelling-fixture-dir;${EPOCANVA_SPELLING_FIXTURE_DIR}")
if(_bridge_baseline_dll)
  list(APPEND _engine_common_args "--baseline-dll;${_bridge_baseline_dll}")
endif()

set(_engine_runtime_args "--mode;runtime" ${_engine_common_args}
    "--run-root;${_engine_run_base}/runtime/engine-bridge-run")
set(_engine_dispatch_args "--mode;dispatch" ${_engine_common_args}
    "--run-root;${_engine_run_base}/dispatch/engine-bridge-run")
set(_engine_guards_args "--mode;guards" ${_engine_common_args}
    "--run-root;${_engine_run_base}/guards/engine-bridge-run")

# 1. Runtime and session lifecycle: the loaded DLL identity, required API slots, explicit base
#    and English modules, two independent sessions, one process-global initialize, a refused
#    second runtime, thread ownership, shutdown ordering, prepared-directory deployment being
#    distinct from runtime use, failure cases, log hygiene and API acquire/free pairing.
add_test(
  NAME windows.engine.runtime_lifecycle
  COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:engine_bridge_tests>
    -DCASE=runtime-lifecycle
    -DEXPECTED_EXIT=0
    "-DMARKER=RESULT PASS"
    -DMARKER_POLICY=require
    "-DPASS_ARGS=${_engine_runtime_args}"
    -DTIMEOUT_SECONDS=300
    -P ${_engine_assert}
)

# 2. Pure result-copy and offset mapping: UTF-8 bytes -> UTF-16 code units, embedded NUL,
#    supplementary characters, and every preedit cursor/selection sentinel and invalid shape
#    (negative npos, past end, inside a UTF-8 sequence, start after end). No engine is loaded.
add_test(
  NAME windows.engine.state_mapping
  COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:engine_bridge_tests>
    -DCASE=state-mapping
    -DEXPECTED_EXIT=0
    "-DMARKER=RESULT PASS"
    -DMARKER_POLICY=require
    "-DPASS_ARGS=--mode;state"
    -DTIMEOUT_SECONDS=120
    -P ${_engine_assert}
)

# 3. Falsifiability control: one deliberately false claim about a malformed UTF-8 byte must
#    exit 14 and must not print RESULT PASS.
add_test(
  NAME windows.engine.runtime_can_fail
  COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:engine_bridge_tests>
    -DCASE=runtime-can-fail
    -DEXPECTED_EXIT=14
    "-DMARKER=RESULT PASS"
    -DMARKER_POLICY=forbid
    "-DPASS_ARGS=--mode;self-check-fail"
    -DTIMEOUT_SECONDS=120
    -P ${_engine_assert}
)

# 4. Controller dispatch against the real English engine: three Evaluate calls do not move
#    the engine, one Dispatch does; h/e/l produce the real menu; uppercase is not shifted
#    twice; BackSpace/Escape/Enter/Space keep their existing engine meanings; UTF-16
#    candidate/comment/commit copies are exact; explicit selection is distinct from the
#    default raw input; a commit is consumed once; repeat_count is not expanded.
add_test(
  NAME windows.engine.key_dispatch
  COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:engine_bridge_tests>
    -DCASE=key-dispatch
    -DEXPECTED_EXIT=0
    "-DMARKER=RESULT PASS"
    -DMARKER_POLICY=require
    "-DPASS_ARGS=${_engine_dispatch_args}"
    -DTIMEOUT_SECONDS=300
    -P ${_engine_assert}
)

# 5. Refusals and isolation: shortcuts/AltGr, key release, forbidden/unknown permission,
#    disabled service, engine-not-ready, Shift+special, boundary deferral leaving the
#    composition intact, two interleaved controllers, and every cross-thread method refused
#    without an engine call.
add_test(
  NAME windows.engine.dispatch_guards
  COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:engine_bridge_tests>
    -DCASE=dispatch-guards
    -DEXPECTED_EXIT=0
    "-DMARKER=RESULT PASS"
    -DMARKER_POLICY=require
    "-DPASS_ARGS=${_engine_guards_args}"
    -DTIMEOUT_SECONDS=300
    -P ${_engine_assert}
)

# 6. Falsifiability control for the controller: a deliberately false claim that Evaluate
#    changes engine state must exit 14 with no RESULT PASS.
add_test(
  NAME windows.engine.dispatch_can_fail
  COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:engine_bridge_tests>
    -DCASE=dispatch-can-fail
    -DEXPECTED_EXIT=14
    "-DMARKER=RESULT PASS"
    -DMARKER_POLICY=forbid
    "-DPASS_ARGS=--mode;self-check-fail-dispatch"
    -DTIMEOUT_SECONDS=120
    -P ${_engine_assert}
)

set_tests_properties(
                     windows.engine.runtime_lifecycle
                     windows.engine.state_mapping
                     windows.engine.runtime_can_fail
                     windows.engine.key_dispatch
                     windows.engine.dispatch_guards
                     windows.engine.dispatch_can_fail
  PROPERTIES TIMEOUT 360
             LABELS "windows;engine;runtime;session;controller;bridge")

# Fault-injection DLL and driver use the bridge's MSVC/static-CRT configuration.
# The fake API only exercises defensive exception handling; it is not evidence that
# the pinned C API normally throws. Poison cases each need their own process.
add_library(engine_runtime_fault_api SHARED runtime_fault_api.cc)
add_executable(engine_runtime_fault_tests runtime_fault_tests.cc)
foreach(_fault_target engine_runtime_fault_api engine_runtime_fault_tests)
  target_compile_definitions(${_fault_target} PRIVATE
    _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
  target_compile_options(${_fault_target} PRIVATE
    /W4 /permissive- /EHsc /Zc:__cplusplus /utf-8)
endforeach()
target_include_directories(engine_runtime_fault_api PRIVATE
  "${EPOCANVA_RIME_INCLUDE_DIR}" "${EPOCANVA_ENGLISH_COMPONENT_DIR}")
target_link_libraries(engine_runtime_fault_tests PRIVATE epocanva_engine_bridge)
add_dependencies(engine_runtime_fault_tests engine_runtime_fault_api)
set(_runtime_fault_modes
  clean initialize_failure initialize_cleanup_throw shutdown_throw module_cleanup_throw)
set(_runtime_fault_mode 0)
foreach(_fault_case IN LISTS _runtime_fault_modes)
  add_test(NAME windows.engine.runtime_fault_${_fault_case}
    COMMAND engine_runtime_fault_tests ${_runtime_fault_mode}
      $<TARGET_FILE:engine_runtime_fault_api> "${CMAKE_CURRENT_BINARY_DIR}")
  set_tests_properties(windows.engine.runtime_fault_${_fault_case}
    PROPERTIES TIMEOUT 30 LABELS "windows;engine;runtime;fault-injection")
  math(EXPR _runtime_fault_mode "${_runtime_fault_mode} + 1")
endforeach()

message(STATUS "  engine_bridge cases : 6 behavioural/control + 5 runtime fault cases")
