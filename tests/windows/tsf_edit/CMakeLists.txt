# EpoCanva P3.7 -- stage 1: the controlled TSF text-store host's verification program.
#
# NOT A STANDALONE PROJECT. This directory is included by platforms/windows/tsf_edit
# (the top-level project that owns the static-CRT setting). The guard below keeps it from
# being configured alone, where it would have no CRT agreement with the layer it verifies.
#
# The program uses the real TSF manager: ole32 for CoInitializeEx/CoCreateInstance, uuid for
# the CLSID/IID definitions (CLSID_TF_ThreadMgr and the msctf interfaces), and user32 for
# the message-only window that defers an asynchronous lock grant. No msctf.lib and no
# registry entry are used.
if(NOT EPOCANVA_TSF_EDIT_TOP_LEVEL)
  message(FATAL_ERROR
    "tests/windows/tsf_edit must be included from platforms/windows/tsf_edit; it is not "
    "configurable alone.")
endif()

add_executable(tsf_edit_tests
  main.cc
  test_host.cc
  test_host.h
  test_text_store.cc
  test_text_store.h)
target_compile_definitions(tsf_edit_tests PRIVATE
  _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_compile_options(tsf_edit_tests PRIVATE
  /W4 /permissive- /EHsc /Zc:__cplusplus /utf-8)
target_include_directories(tsf_edit_tests PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(tsf_edit_tests PRIVATE ole32 uuid user32)

enable_testing()

set(_tsf_edit_assert "${CMAKE_CURRENT_LIST_DIR}/../../english_completion/assert_exit.cmake")

function(epocanva_add_tsf_edit_case name case)
  add_test(
    NAME ${name}
    COMMAND ${CMAKE_COMMAND}
      -DEXE=$<TARGET_FILE:tsf_edit_tests>
      -DCASE=${case}
      -DEXPECTED_EXIT=0
      "-DMARKER=RESULT PASS"
      -DMARKER_POLICY=require
      "-DPASS_ARGS=--case;${case}"
      -DTIMEOUT_SECONDS=180
      -P ${_tsf_edit_assert})
endfunction()

# 1. The synchronous path: a real ThreadMgr/document/context, a real RequestEditSession, a
#    real write lock observed by the store, TSF's own insertion reaching the backing text.
epocanva_add_tsf_edit_case(
  windows.tsf_edit.host_sync_edit host-sync-edit)

# 2. The asynchronous path: the store defers the lock, RequestEditSession reports
#    TF_S_ASYNC, the text is untouched until the STA message loop dispatches the posted
#    grant, and only then does DoEditSession run.
epocanva_add_tsf_edit_case(
  windows.tsf_edit.host_async_edit host-async-edit)

# 3. The honest refusal surface: no layout, no window, no embedded objects, no attributes.
epocanva_add_tsf_edit_case(
  windows.tsf_edit.host_unsupported_surface host-unsupported-surface)

# 4. Complete teardown with no surviving store, owner sink or advised sink.
epocanva_add_tsf_edit_case(
  windows.tsf_edit.host_cleanup host-cleanup)

# 5. The falsifiability control: one deliberately false claim, expected exit 14 and no
#    RESULT PASS. Without it, a suite whose checks were tautologies would look identical to
#    a green one.
add_test(
  NAME windows.tsf_edit.host_can_fail
  COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:tsf_edit_tests>
    -DCASE=host-can-fail
    -DEXPECTED_EXIT=14
    "-DMARKER=RESULT PASS"
    -DMARKER_POLICY=forbid
    "-DPASS_ARGS=--case;host-can-fail"
    -DTIMEOUT_SECONDS=180
    -P ${_tsf_edit_assert})

set_tests_properties(
  windows.tsf_edit.host_sync_edit
  windows.tsf_edit.host_async_edit
  windows.tsf_edit.host_unsupported_surface
  windows.tsf_edit.host_cleanup
  windows.tsf_edit.host_can_fail
  PROPERTIES TIMEOUT 240
             LABELS "windows;tsf;edit;host;textstore;lock;async")

message(STATUS "  tsf_edit host cases  : 5 (sync, async, unsupported surface, cleanup, "
               "can_fail)")
