# EpoCanva P3.6 -- broker/lease tests, linked directly against the production broker source.
#
# NOT A STANDALONE PROJECT. This directory is included by
# platforms/windows/engine_bridge/CMakeLists.txt after epocanva_engine_executor exists.
# The production EngineHostBroker (platforms/windows/tsf/engine_host.cc) is compiled from
# its one source into this executable; it is not reimplemented here. The test uses the real
# English rime.dll through the executor; there is no controlled backend in this target.
#
# The test-only gates and the forced-cleanup-failure switch are enabled by
# EPOCANVA_TSF_TEST_SEAM. They are compiled into this executable only; no product target
# defines that macro.
if(NOT TARGET epocanva_engine_executor)
  message(FATAL_ERROR
    "tests/windows/tsf_engine_host must be included from platforms/windows/engine_bridge; "
    "it links epocanva_engine_executor.")
endif()

set(EpoCanva_Tsf_Source_Dir
    "${CMAKE_CURRENT_LIST_DIR}/../../../platforms/windows/tsf")

add_executable(tsf_engine_host_tests
  main.cc
  "${EpoCanva_Tsf_Source_Dir}/engine_host.cc"
  "${EpoCanva_Tsf_Source_Dir}/engine_host.h"
  "${EpoCanva_Tsf_Source_Dir}/module_state.cc"
  "${EpoCanva_Tsf_Source_Dir}/module_state.h")
target_compile_definitions(tsf_engine_host_tests PRIVATE
  _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
  EPOCANVA_TSF_TEST_SEAM=1)
target_compile_options(tsf_engine_host_tests PRIVATE
  /W4 /permissive- /EHsc /Zc:__cplusplus /utf-8)
target_include_directories(tsf_engine_host_tests PRIVATE
  "${EpoCanva_Tsf_Source_Dir}")
target_link_libraries(tsf_engine_host_tests PRIVATE epocanva_engine_executor)

enable_testing()

set(_host_assert "${CMAKE_CURRENT_LIST_DIR}/../../english_completion/assert_exit.cmake")
set(_host_run_base "${CMAKE_CURRENT_BINARY_DIR}/run")

set(_host_args
  "--rime-dll;${EPOCANVA_RIME_DLL}"
  "--fixture-dir;${EPOCANVA_ENGLISH_FIXTURE_DIR}"
  "--spelling-fixture-dir;${EPOCANVA_SPELLING_FIXTURE_DIR}")
if(_bridge_baseline_dll)
  list(APPEND _host_args "--baseline-dll" "${_bridge_baseline_dll}")
endif()

# 1. Real broker/lease lifecycle: one start per lease set, shared leases, last-release stop
#    and join, a new executor instance after a clean stop, configuration mismatch refusal,
#    Acquire rejection during Stopping, clean startup failure with retry, missing-module
#    failure with retry, lease double-release with no underflow, single starter with two
#    waiting Acquire calls, and the forced cleanup-failure latch.
add_test(
  NAME windows.tsf_host.broker_lifecycle
  COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:tsf_engine_host_tests>
    -DCASE=broker-lifecycle
    -DEXPECTED_EXIT=0
    "-DMARKER=RESULT PASS"
    -DMARKER_POLICY=require
    "-DPASS_ARGS=--mode;broker;${_host_args};--work-root;${_host_run_base}/broker/tsf-host-run"
    -DTIMEOUT_SECONDS=300
    -P ${_host_assert}
)

# 2. Falsifiability control. It makes one deliberately false claim and must exit 14 without
#    printing RESULT PASS.
add_test(
  NAME windows.tsf_host.broker_can_fail
  COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:tsf_engine_host_tests>
    -DCASE=broker-can-fail
    -DEXPECTED_EXIT=14
    "-DMARKER=RESULT PASS"
    -DMARKER_POLICY=forbid
    "-DPASS_ARGS=--mode;self-check-fail"
    -DTIMEOUT_SECONDS=120
    -P ${_host_assert}
)

set_tests_properties(windows.tsf_host.broker_lifecycle
                     windows.tsf_host.broker_can_fail
  PROPERTIES TIMEOUT 360
             LABELS "windows;tsf;engine;host;broker;lease;lifecycle;concurrency")

message(STATUS "  tsf_engine_host cases: 2 (broker_lifecycle, broker_can_fail)")
