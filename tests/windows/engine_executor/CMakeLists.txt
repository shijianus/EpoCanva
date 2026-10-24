# EpoCanva P3.5 -- engine executor scheduling-layer verification and its CTest cases.
#
# NOT A STANDALONE PROJECT. This directory is included by
# platforms/windows/engine_bridge/CMakeLists.txt. This stage uses a controlled backend and does
# not load rime.dll. The real-engine cases are registered together with the real-backend
# verification.
if(NOT TARGET epocanva_engine_executor)
  message(FATAL_ERROR
    "tests/windows/engine_executor must be included from platforms/windows/engine_bridge; it "
    "links epocanva_engine_executor.")
endif()

add_executable(engine_executor_tests
  main.cc
  test_support.cc
  test_support.h
  controlled_backend.cc
  controlled_backend.h
  scheduler_tests.cc)
target_compile_definitions(engine_executor_tests PRIVATE
  _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_compile_options(engine_executor_tests PRIVATE
  /W4 /permissive- /EHsc /Zc:__cplusplus /utf-8)
target_link_libraries(engine_executor_tests PRIVATE
  epocanva_engine_executor epocanva_engine_keyboard_adapters)

enable_testing()

set(_executor_assert
    "${CMAKE_CURRENT_LIST_DIR}/../../english_completion/assert_exit.cmake")
set(_executor_run_base "${CMAKE_CURRENT_BINARY_DIR}/run")

# 1. Controlled backend: lifecycle/state machine, Start/Stop race admission, non-restart,
#    FIFO, client tokens, close admission, capacity in all three states, result consumption,
#    timeout (no cancellation), stop draining, self-join refusal, per-command exception,
#    executor-wide start/cleanup failures and invalid requests. No engine is loaded.
add_test(
  NAME windows.executor.scheduler_lifecycle
  COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:engine_executor_tests>
    -DCASE=scheduler-lifecycle
    -DEXPECTED_EXIT=0
    "-DMARKER=RESULT PASS"
    -DMARKER_POLICY=require
    "-DPASS_ARGS=--mode;scheduler;--run-root;${_executor_run_base}/scheduler/engine-executor-run"
    -DTIMEOUT_SECONDS=180
    -P ${_executor_assert}
)

# 2. Falsifiability control for the scheduler suite: an unknown client must not be accepted.
add_test(
  NAME windows.executor.scheduler_can_fail
  COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:engine_executor_tests>
    -DCASE=scheduler-can-fail
    -DEXPECTED_EXIT=14
    "-DMARKER=RESULT PASS"
    -DMARKER_POLICY=forbid
    "-DPASS_ARGS=--mode;self-check-fail"
    -DTIMEOUT_SECONDS=120
    -P ${_executor_assert}
)

set_tests_properties(
                     windows.executor.scheduler_lifecycle
                     windows.executor.scheduler_can_fail
  PROPERTIES TIMEOUT 360
             LABELS "windows;engine;executor;lifecycle;capacity")

message(STATUS "  engine_executor scheduler cases : 2 "
               "(scheduler_lifecycle, scheduler_can_fail)")
