# EpoCanva P3.6 -- hosted TEST DLL variant and its verification programs.
#
# NOT A STANDALONE PROJECT. This directory is included by
# platforms/windows/engine_bridge/CMakeLists.txt after epocanva_engine_executor exists.
#
# WHAT IS BUILT
#   epocanva_tsf_hosted_test  a DLL compiled from the SAME production TSF sources as the
#                             standalone skeleton (dll_main, class_factory, text_service,
#                             module_state, identifiers) plus engine_host.cc. The two
#                             EPOCANVA_TSF_ENGINE_HOST / EPOCANVA_TSF_TEST_SEAM defines are
#                             the only differences; the seam translation unit and .def are
#                             test-only. The product epocanva_tsf target and its .def are not
#                             touched, and no product public export is added.
#   tsf_hosted_tests          the hosted lifecycle program (real ThreadMgrs, real English
#                             rime.dll through the executor).
#   tsf_lifecycle_hosted_compat  the original P3.1 lifecycle program, compiled from the same
#                             tests/windows/tsf_lifecycle/main.cc, run against the hosted DLL
#                             to prove the class factory / LockServer / wrong-IID behaviour
#                             did not regress.
#
# The real product configuration and deployment are NOT implemented here: the test seam is
# the only caller of SetEngineHostConfig. Static CRT /MT is inherited from the engine_bridge
# top-level project, so linking the executor does not pull a second CRT into the DLL.
if(NOT TARGET epocanva_engine_executor)
  message(FATAL_ERROR
    "tests/windows/tsf_lifecycle/hosted must be included from "
    "platforms/windows/engine_bridge; it links epocanva_engine_executor.")
endif()

set(EpoCanva_Tsf_Source_Dir
    "${CMAKE_CURRENT_LIST_DIR}/../../../../platforms/windows/tsf")
set(EpoCanva_Engine_Bridge_Dir
    "${CMAKE_CURRENT_LIST_DIR}/../../../../platforms/windows/engine_bridge")
set(EpoCanva_Engine_Executor_Dir
    "${CMAKE_CURRENT_LIST_DIR}/../../../../platforms/windows/engine_executor")
set(EpoCanva_Input_Dir
    "${CMAKE_CURRENT_LIST_DIR}/../../../../platforms/windows/input")
set(EpoCanva_Text_Dir
    "${CMAKE_CURRENT_LIST_DIR}/../../../../platforms/windows/text")

set(_tsf_definitions
  _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
set(_tsf_options /W4 /permissive- /EHsc /Zc:__cplusplus /utf-8)

add_library(epocanva_tsf_hosted_test SHARED
  "${EpoCanva_Tsf_Source_Dir}/dll_main.cc"
  "${EpoCanva_Tsf_Source_Dir}/identifiers.cc"
  "${EpoCanva_Tsf_Source_Dir}/module_state.cc"
  "${EpoCanva_Tsf_Source_Dir}/registration.cc"
  "${EpoCanva_Tsf_Source_Dir}/class_factory.cc"
  "${EpoCanva_Tsf_Source_Dir}/text_service.cc"
  "${EpoCanva_Tsf_Source_Dir}/engine_host.cc"
  "${CMAKE_CURRENT_LIST_DIR}/test_seam.cc"
  "${CMAKE_CURRENT_LIST_DIR}/test_seam_abi.h"
  "${CMAKE_CURRENT_LIST_DIR}/epocanva_tsf_hosted_test.def")
target_compile_definitions(epocanva_tsf_hosted_test PRIVATE
  ${_tsf_definitions}
  EPOCANVA_TSF_ENGINE_HOST=1
  EPOCANVA_TSF_TEST_SEAM=1)
target_compile_options(epocanva_tsf_hosted_test PRIVATE ${_tsf_options})
target_include_directories(epocanva_tsf_hosted_test PRIVATE
  "${EpoCanva_Tsf_Source_Dir}"
  "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(epocanva_tsf_hosted_test PRIVATE
  epocanva_engine_executor ole32 uuid)

add_executable(tsf_hosted_tests
  hosted_main.cc
  "${EpoCanva_Tsf_Source_Dir}/identifiers.cc")
target_compile_definitions(tsf_hosted_tests PRIVATE ${_tsf_definitions})
target_compile_options(tsf_hosted_tests PRIVATE ${_tsf_options})
target_include_directories(tsf_hosted_tests PRIVATE
  "${EpoCanva_Tsf_Source_Dir}"
  "${CMAKE_CURRENT_LIST_DIR}"
  "${EpoCanva_Engine_Bridge_Dir}"
  "${EpoCanva_Engine_Executor_Dir}"
  "${EpoCanva_Input_Dir}"
  "${EpoCanva_Text_Dir}")
target_link_libraries(tsf_hosted_tests PRIVATE ole32 uuid)
add_dependencies(tsf_hosted_tests epocanva_tsf_hosted_test)

add_executable(tsf_lifecycle_hosted_compat
  "${CMAKE_CURRENT_LIST_DIR}/../main.cc"
  "${EpoCanva_Tsf_Source_Dir}/identifiers.cc")
target_compile_definitions(tsf_lifecycle_hosted_compat PRIVATE ${_tsf_definitions})
target_compile_options(tsf_lifecycle_hosted_compat PRIVATE ${_tsf_options})
target_include_directories(tsf_lifecycle_hosted_compat PRIVATE
  "${EpoCanva_Tsf_Source_Dir}")
target_link_libraries(tsf_lifecycle_hosted_compat PRIVATE ole32 uuid)
add_dependencies(tsf_lifecycle_hosted_compat epocanva_tsf_hosted_test)

enable_testing()

set(_hosted_assert
    "${CMAKE_CURRENT_LIST_DIR}/../../../english_completion/assert_exit.cmake")
set(_hosted_run_base "${CMAKE_CURRENT_BINARY_DIR}/run")
set(_hosted_dll "$<TARGET_FILE:epocanva_tsf_hosted_test>")

set(_hosted_engine_args
  "--rime-dll;${EPOCANVA_RIME_DLL}"
  "--fixture-dir;${EPOCANVA_ENGLISH_FIXTURE_DIR}"
  "--spelling-fixture-dir;${EPOCANVA_SPELLING_FIXTURE_DIR}")
if(_bridge_baseline_dll)
  list(APPEND _hosted_engine_args "--baseline-dll" "${_bridge_baseline_dll}")
endif()

function(epocanva_add_hosted_case case_name mode)
  add_test(
    NAME "tsf.hosted.${case_name}"
    COMMAND ${CMAKE_COMMAND}
      -DEXE=$<TARGET_FILE:tsf_hosted_tests>
      -DCASE=hosted-${case_name}
      -DEXPECTED_EXIT=0
      "-DMARKER=RESULT PASS"
      -DMARKER_POLICY=require
      "-DPASS_ARGS=--dll;${_hosted_dll};--mode;${mode};${_hosted_engine_args};--work-root;${_hosted_run_base}/${case_name}/tsf-host-run"
      -DTIMEOUT_SECONDS=300
      -P ${_hosted_assert}
  )
endfunction()

# 1. One service: one Runtime, same-ThreadMgr reactivation, clean stop, new instance after
#    the stop.
epocanva_add_hosted_case(single_service single)

# 2. Two services sharing one executor; one deactivation leaves the other usable; a
#    different configuration is refused without disturbing the active service.
epocanva_add_hosted_case(shared_services shared)

# 3. Two STA threads, two real ThreadMgrs, two services, one shared broker. No COM pointer
#    crosses apartments.
epocanva_add_hosted_case(multi_sta multista)

# 4. Start/stop/cleanup gates: DllCanUnloadNow is S_FALSE for the whole window, Acquire
#    during Stopping is refused, S_OK only after every object is released.
epocanva_add_hosted_case(unload_window unload)

# 5. Controlled cleanup-failure latch: zero live objects and zero locks are not enough.
epocanva_add_hosted_case(cleanup_failure cleanup)

# 6. Missing DLL and missing English module: clean failure, retry with a new executor.
epocanva_add_hosted_case(startup_failure startup)

# 7. Lease double release cannot underflow.
epocanva_add_hosted_case(lease_underflow lease)

# 8. The original P3.1 COM contract (class factory, LockServer, wrong IID, five cycles)
#    against the hosted DLL.
add_test(
  NAME tsf.hosted.original_com_contract
  COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:tsf_lifecycle_hosted_compat>
    -DCASE=hosted-original-com-contract
    -DEXPECTED_EXIT=0
    "-DMARKER=RESULT PASS"
    -DMARKER_POLICY=require
    "-DPASS_ARGS=--dll;${_hosted_dll};--mode;lifecycle"
    -DTIMEOUT_SECONDS=180
    -P ${_hosted_assert}
)

# 9. Falsifiability control for the hosted suite.
add_test(
  NAME tsf.hosted.suite_can_fail
  COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:tsf_hosted_tests>
    -DCASE=hosted-can-fail
    -DEXPECTED_EXIT=14
    "-DMARKER=RESULT PASS"
    -DMARKER_POLICY=forbid
    "-DPASS_ARGS=--dll;${_hosted_dll};--mode;self-check-fail"
    -DTIMEOUT_SECONDS=120
    -P ${_hosted_assert}
)

set_tests_properties(
  tsf.hosted.single_service
  tsf.hosted.shared_services
  tsf.hosted.multi_sta
  tsf.hosted.unload_window
  tsf.hosted.cleanup_failure
  tsf.hosted.startup_failure
  tsf.hosted.lease_underflow
  tsf.hosted.original_com_contract
  tsf.hosted.suite_can_fail
  PROPERTIES TIMEOUT 360
             LABELS "windows;tsf;hosted;engine;lifecycle;concurrency")

message(STATUS "  tsf_hosted cases     : 9 (single_service, shared_services, multi_sta, "
               "unload_window, cleanup_failure, startup_failure, lease_underflow, "
               "original_com_contract, suite_can_fail)")
