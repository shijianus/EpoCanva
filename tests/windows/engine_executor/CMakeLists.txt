# EpoCanva P3.5 -- engine executor verification program and its CTest cases.
#
# NOT A STANDALONE PROJECT. This directory is included by
# platforms/windows/engine_bridge/CMakeLists.txt. The scheduler cases use a controlled backend
# and never load rime.dll. The engine/race/startup-failure cases use the executor's real
# backend and the already-built English rime.dll named by EPOCANVA_RIME_DLL. The two layers are
# separate executables runs and separate named cases.
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
  scheduler_tests.cc
  real_engine_tests.cc)
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

set(_executor_engine_args
  "--rime-dll;${EPOCANVA_RIME_DLL}"
  "--fixture-dir;${EPOCANVA_ENGLISH_FIXTURE_DIR}"
  "--spelling-fixture-dir;${EPOCANVA_SPELLING_FIXTURE_DIR}")
if(_bridge_baseline_dll)
  list(APPEND _executor_engine_args "--baseline-dll;${_bridge_baseline_dll}")
endif()

# 1. Controlled backend: lifecycle/state machine, non-restart, FIFO, client tokens, close
#    admission, capacity in all three states, result consumption, timeout (no cancellation),
#    stop draining, self-join refusal, per-command exception, executor-wide start/cleanup
#    failures and invalid requests. No engine is loaded.
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

# 3. Real English engine through the executor: runtime start/shutdown thread, one thread for
#    every command, two caller threads with two clients, same-client order, pure Evaluate,
#    letters/BackSpace/Return/Space, explicit candidate, boundary/shortcut/forbidden refusals,
#    one commit, close admission and preserved results, capacity with real sessions, process
#    exclusion and a clean Stop with no live sessions.
add_test(
  NAME windows.executor.real_engine
  COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:engine_executor_tests>
    -DCASE=real-engine
    -DEXPECTED_EXIT=0
    "-DMARKER=RESULT PASS"
    -DMARKER_POLICY=require
    "-DPASS_ARGS=--mode;engine;${_executor_engine_args};--run-root;${_executor_run_base}/engine/engine-executor-run"
    -DTIMEOUT_SECONDS=300
    -P ${_executor_assert}
)

# 4. Concurrent submissions from four threads race StopAndJoin. Every accepted request must
#    have a terminal result; every rejected submission must be an explicit status.
add_test(
  NAME windows.executor.stop_race
  COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:engine_executor_tests>
    -DCASE=stop-race
    -DEXPECTED_EXIT=0
    "-DMARKER=RESULT PASS"
    -DMARKER_POLICY=require
    "-DPASS_ARGS=--mode;race;${_executor_engine_args};--run-root;${_executor_run_base}/race/engine-executor-run"
    -DTIMEOUT_SECONDS=300
    -P ${_executor_assert}
)

# 5. Missing/relative DLL and the baseline DLL without the English module: the worker reports
#    the real RuntimeStartStatus, the object is terminal, and a failed initialization is
#    finalized so a later executor can start.
add_test(
  NAME windows.executor.startup_failure
  COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:engine_executor_tests>
    -DCASE=startup-failure
    -DEXPECTED_EXIT=0
    "-DMARKER=RESULT PASS"
    -DMARKER_POLICY=require
    "-DPASS_ARGS=--mode;startup-failure;${_executor_engine_args};--run-root;${_executor_run_base}/startup/engine-executor-run"
    -DTIMEOUT_SECONDS=300
    -P ${_executor_assert}
)

set_tests_properties(
                     windows.executor.scheduler_lifecycle
                     windows.executor.scheduler_can_fail
                     windows.executor.real_engine
                     windows.executor.stop_race
                     windows.executor.startup_failure
  PROPERTIES TIMEOUT 360
             LABELS "windows;engine;executor;lifecycle;capacity;concurrency")

message(STATUS "  engine_executor cases : 5 (scheduler_lifecycle, scheduler_can_fail, "
               "real_engine, stop_race, startup_failure)")
