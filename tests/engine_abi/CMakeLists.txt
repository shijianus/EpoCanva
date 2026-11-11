cmake_minimum_required(VERSION 3.25)
# Both entry points are supported: standalone verification, and add_subdirectory
# from the product bridge (which owns the CRT and existing runtime target).
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(epocanva_engine_abi_tests VERSION 0.1.0 LANGUAGES C CXX)
  set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
else()
  get_property(_abi_languages GLOBAL PROPERTY ENABLED_LANGUAGES)
  if(NOT "C" IN_LIST _abi_languages)
    enable_language(C)
  endif()
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
get_filename_component(_repo "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
if(NOT TARGET epocanva_engine_api)
  add_subdirectory("${_repo}/engine/core" "${CMAKE_CURRENT_BINARY_DIR}/engine_core")
endif()

set(EPOCANVA_RIME_DLL "${_repo}/build/librime-english/rime/install/lib/rime.dll"
    CACHE FILEPATH "Existing pinned English Rime DLL; never downloaded or built here")
set(EPOCANVA_RIME_RECEIPT "${_repo}/build/librime-english/engine-build-receipt.json"
    CACHE FILEPATH "Current receipt that binds the DLL SHA-256 when receipt checking is enabled")
option(EPOCANVA_ENGINE_ABI_VERIFY_RECEIPT
       "Verify the current Rime DLL hash against its current build receipt" ON)
foreach(_required "${EPOCANVA_RIME_DLL}" "${_repo}/third_party/librime/src/rime_api.h")
  if(NOT EXISTS "${_required}")
    message(FATAL_ERROR "Required real-engine input is absent: ${_required}; tests cannot be reported green")
  endif()
endforeach()
file(SHA256 "${EPOCANVA_RIME_DLL}" _actual_dll_hash)
if(EPOCANVA_ENGINE_ABI_VERIFY_RECEIPT)
  if(NOT EXISTS "${EPOCANVA_RIME_RECEIPT}")
    message(FATAL_ERROR "Current Rime build receipt is absent: ${EPOCANVA_RIME_RECEIPT}")
  endif()
  file(READ "${EPOCANVA_RIME_RECEIPT}" _receipt)
  string(JSON _receipt_status GET "${_receipt}" status)
  string(JSON _expected_dll_hash GET "${_receipt}" rime_sha256)
  if(NOT _receipt_status STREQUAL "built" OR NOT _actual_dll_hash STREQUAL _expected_dll_hash)
    message(FATAL_ERROR "Current Rime DLL does not match its built receipt; rebuild/reissue receipt, then configure again")
  endif()
else()
  message(STATUS "ABI tests bind the configure-time DLL SHA-256 only; no source/build-receipt attestation")
endif()
find_package(Git REQUIRED)
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${_repo}/third_party/librime" rev-parse HEAD
  RESULT_VARIABLE _git_result OUTPUT_VARIABLE _rime_commit OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _git_result EQUAL 0 OR NOT _rime_commit STREQUAL "33e78140250125871856cdc5b42ddc6a5fcd3cd4")
  message(FATAL_ERROR "The actual Rime source checkout is not the pinned revision")
endif()
message(STATUS "Rime source pin: ${_rime_commit}")
message(STATUS "Rime DLL verified SHA-256: ${_actual_dll_hash}")

add_executable(engine_abi_c_consumer c_consumer.c)
target_link_libraries(engine_abi_c_consumer PRIVATE epocanva_engine_api)
target_compile_options(engine_abi_c_consumer PRIVATE /W4 /WX /TC /utf-8)
# C source is compiled as C; linking uses C++ because the implementation is C++.
set_target_properties(engine_abi_c_consumer PROPERTIES LINKER_LANGUAGE CXX)

add_executable(engine_abi_tests contract_tests.cc)
target_link_libraries(engine_abi_tests PRIVATE epocanva_engine_api)
target_compile_options(engine_abi_tests PRIVATE /W4 /WX /permissive- /EHsc /Zc:__cplusplus /utf-8)
target_compile_definitions(engine_abi_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)

# Capture verification material only in build/<preset>.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/rime-identity.txt"
  "pinned_commit=${_rime_commit}\nrime_sha256=${_actual_dll_hash}\nrime_version=1.17.0\n")
enable_testing()
set(_assert "${_repo}/tests/english_completion/assert_exit.cmake")
function(_abi_case name mode expected marker_policy)
  add_test(NAME "engine_abi.${name}"
    COMMAND "${CMAKE_COMMAND}" "-DEXE=$<TARGET_FILE:engine_abi_tests>"
      "-DCASE=${name}" "-DEXPECTED_EXIT=${expected}" "-DMARKER=RESULT PASS"
      "-DMARKER_POLICY=${marker_policy}"
      "-DRIME_DLL=${EPOCANVA_RIME_DLL}" "-DEXPECTED_RIME_SHA256=${_actual_dll_hash}"
      "-DPASS_ARGS=--mode;${mode};--rime-dll;${EPOCANVA_RIME_DLL};--repo;${_repo};--run-root;${CMAKE_CURRENT_BINARY_DIR}/run/${name}"
      -DTIMEOUT_SECONDS=180 -P "${CMAKE_CURRENT_SOURCE_DIR}/assert_current_rime.cmake")
  set_tests_properties("engine_abi.${name}" PROPERTIES TIMEOUT 210 LABELS "engine;abi;real-rime")
endfunction()
_abi_case(contracts real 0 require)
_abi_case(missing_dll missing-dll 0 require)
_abi_case(wrong_rime_version wrong-version 0 require)
_abi_case(abi_can_fail control-abi 14 forbid)
_abi_case(utf_can_fail control-utf 14 forbid)
_abi_case(privacy_can_fail control-privacy 14 forbid)
_abi_case(stale_candidate_can_fail control-stale 14 forbid)
add_test(NAME engine_abi.pure_c
  COMMAND "${CMAKE_COMMAND}" "-DEXE=$<TARGET_FILE:engine_abi_c_consumer>"
    -DCASE=pure-c -DEXPECTED_EXIT=0 "-DMARKER=RESULT PASS" -DMARKER_POLICY=require
    "-DPASS_ARGS=run" -P "${_assert}")
add_test(NAME engine_abi.pure_c_can_fail
  COMMAND "${CMAKE_COMMAND}" "-DEXE=$<TARGET_FILE:engine_abi_c_consumer>"
    -DCASE=pure-c-can-fail -DEXPECTED_EXIT=14 "-DMARKER=RESULT PASS" -DMARKER_POLICY=forbid
    "-DPASS_ARGS=control" -P "${_assert}")
