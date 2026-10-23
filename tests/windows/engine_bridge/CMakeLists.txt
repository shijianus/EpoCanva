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

add_executable(engine_bridge_tests main.cc)
target_compile_definitions(engine_bridge_tests PRIVATE
  _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_compile_options(engine_bridge_tests PRIVATE
  /W4 /permissive- /EHsc /Zc:__cplusplus /utf-8)
target_link_libraries(engine_bridge_tests PRIVATE epocanva_engine_bridge)

enable_testing()

set(_engine_assert "${CMAKE_CURRENT_LIST_DIR}/../../english_completion/assert_exit.cmake")
set(_engine_run_base "${CMAKE_CURRENT_BINARY_DIR}/run")

set(_engine_runtime_args
  "--mode;runtime"
  "--rime-dll;${EPOCANVA_RIME_DLL}"
  "--fixture-dir;${EPOCANVA_ENGLISH_FIXTURE_DIR}"
  "--spelling-fixture-dir;${EPOCANVA_SPELLING_FIXTURE_DIR}"
  "--run-root;${_engine_run_base}/runtime/engine-bridge-run")
if(_bridge_baseline_dll)
  list(APPEND _engine_runtime_args "--baseline-dll;${_bridge_baseline_dll}")
endif()

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

set_tests_properties(
                     windows.engine.runtime_lifecycle
                     windows.engine.state_mapping
                     windows.engine.runtime_can_fail
  PROPERTIES TIMEOUT 360
             LABELS "windows;engine;runtime;session;bridge")

message(STATUS "  engine_bridge cases : 3 (runtime_lifecycle, state_mapping, runtime_can_fail)")
