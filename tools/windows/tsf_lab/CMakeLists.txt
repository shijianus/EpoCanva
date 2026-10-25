# EpoCanva P3.11/P3.13 -- the visible TSF integration lab.
#
# NOT A STANDALONE PROJECT. It is added by platforms/windows/engine_bridge only when
# EPOCANVA_BUILD_TSF_LAB=ON, so the normal CTest tree never opens an interactive window.
#
# WHAT IS REUSED, NOT REBUILT
#   epocanva_tsf_coordinator   P3.8/P3.9 coordinator + P3.7 edit layer
#   epocanva_engine_executor   P3.5 engine executor and the real English engine backend
#   epocanva_tsf_context       P3.9 permission source + P3.10 observer/registry
#   tests/windows/tsf_edit     the P3.7 controlled TSF host and ACP text store
#   tests/windows/engine_executor  fixed-fixture deployment + value-free checks
#   tests/windows/tsf_context_observer  the distinct real external client helper
#
# No second text store, key translator, engine bridge or language implementation is
# written here. The lab is a test host, not an input method: it registers nothing with the
# OS and installs no keyboard sink.
if(NOT TARGET epocanva_tsf_coordinator)
  message(FATAL_ERROR
    "tools/windows/tsf_lab must be added by platforms/windows/engine_bridge; it links "
    "the existing coordinator/executor/context targets.")
endif()

set(_lab_tsf_edit_tests_dir "${CMAKE_CURRENT_LIST_DIR}/../../../tests/windows/tsf_edit")
set(_lab_observer_tests_dir
    "${CMAKE_CURRENT_LIST_DIR}/../../../tests/windows/tsf_context_observer")
set(_lab_executor_tests_dir
    "${CMAKE_CURRENT_LIST_DIR}/../../../tests/windows/engine_executor")

add_executable(tsf_lab
  main.cc
  lab_window.cc
  lab_window.h
  lab_session.cc
  lab_session.h
  scenario_runner.cc
  scenario_runner.h
  lab_self_test.cc
  lab_self_test.h
  "${_lab_observer_tests_dir}/observer_test_support.cc"
  "${_lab_observer_tests_dir}/observer_test_support.h"
  "${_lab_tsf_edit_tests_dir}/test_host.cc"
  "${_lab_tsf_edit_tests_dir}/test_host.h"
  "${_lab_tsf_edit_tests_dir}/test_text_store.cc"
  "${_lab_tsf_edit_tests_dir}/test_text_store.h"
  "${_lab_executor_tests_dir}/test_support.cc"
  "${_lab_executor_tests_dir}/test_support.h")
target_compile_definitions(tsf_lab PRIVATE
  _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
  EPOCANVA_LAB_DEFAULT_RIME_DLL="${EPOCANVA_RIME_DLL}"
  EPOCANVA_LAB_DEFAULT_FIXTURE_DIR="${EPOCANVA_ENGLISH_FIXTURE_DIR}"
  EPOCANVA_LAB_DEFAULT_SPELLING_FIXTURE_DIR="${EPOCANVA_SPELLING_FIXTURE_DIR}"
  EPOCANVA_LAB_DEFAULT_RUN_ROOT="${CMAKE_CURRENT_BINARY_DIR}/run")
target_compile_options(tsf_lab PRIVATE
  /W4 /permissive- /EHsc /Zc:__cplusplus /utf-8)
target_include_directories(tsf_lab PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}"
  "${_lab_tsf_edit_tests_dir}"
  "${_lab_observer_tests_dir}"
  # tests/windows so "engine_executor/test_support.h" is unambiguous.
  "${CMAKE_CURRENT_LIST_DIR}/../../../tests/windows")
target_link_libraries(tsf_lab PRIVATE
  epocanva_tsf_coordinator epocanva_engine_keyboard_adapters ole32 uuid user32)

enable_testing()

set(_lab_assert
  "${CMAKE_CURRENT_LIST_DIR}/../../../tests/english_completion/assert_exit.cmake")

# No window is shown by these cases. The lifecycle case needs no engine; the scenarios
# case creates a hidden window, starts the real TSF host and the pinned English engine,
# drives all nine fixed scripts with programmatic ITfThreadMgr::SetFocus, and checks the
# reference/runtime balance.
add_test(
  NAME windows.tsf_lab.window_lifecycle
  COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:tsf_lab>
    -DCASE=window-lifecycle
    -DEXPECTED_EXIT=0
    "-DMARKER=RESULT PASS"
    -DMARKER_POLICY=require
    "-DPASS_ARGS=--self-test;--case;window-lifecycle"
    -DTIMEOUT_SECONDS=120
    -P ${_lab_assert})
set_tests_properties(
  windows.tsf_lab.window_lifecycle
  PROPERTIES TIMEOUT 180
             LABELS "windows;tsf;lab;window;lifecycle;no-ui")

set(_lab_run_base "${CMAKE_CURRENT_BINARY_DIR}/run/scenarios")
add_test(
  NAME windows.tsf_lab.scenarios
  COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:tsf_lab>
    -DCASE=scenarios
    -DEXPECTED_EXIT=0
    "-DMARKER=RESULT PASS"
    -DMARKER_POLICY=require
    "-DPASS_ARGS=--self-test;--case;scenarios;--rime-dll;${EPOCANVA_RIME_DLL};--fixture-dir;${EPOCANVA_ENGLISH_FIXTURE_DIR};--spelling-fixture-dir;${EPOCANVA_SPELLING_FIXTURE_DIR};--run-root;${_lab_run_base}"
    -DTIMEOUT_SECONDS=600
    -P ${_lab_assert})
set_tests_properties(
  windows.tsf_lab.scenarios
  PROPERTIES TIMEOUT 660
             LABELS "windows;tsf;lab;scenario;engine;english;real;no-ui")

message(STATUS
  "  tsf_lab: add_test window_lifecycle + scenarios (hidden windows, no interactive CTest)")
