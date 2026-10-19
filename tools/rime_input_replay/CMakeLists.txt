# EpoCanva P1.3 -- standalone project for the headless librime input-link replay.
#
# Deliberately NOT add_subdirectory()'d into the root EpoCanva project, for the same
# reason tools/rime_lifecycle_smoke is not: the root project is a Debug / dynamic-CRT
# probe, while this program must link the Release / static-CRT librime import library
# produced by tools/build_librime_baseline.ps1. Separate projects keep the two CRTs
# from leaking into each other and keep the third-party chain out of the repository's
# own configure step.
#
# Nothing in tools/rime_lifecycle_smoke was modified to build this: the ~90 lines of
# platform helpers that the two programs share are duplicated in main.cc rather than
# factored into a static library, because a shared target would add a third build
# graph edge (and a second place where the CRT has to be kept in agreement) to save
# ninety lines.
#
# Configure with:
#   cmake -S tools/rime_input_replay -B build/librime-baseline/replay \
#         -DRime_Install_Prefix=<repo>/build/librime-baseline/rime/install

cmake_minimum_required(VERSION 3.25)
project(epocanva_rime_input_replay VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  message(FATAL_ERROR
    "This project must be configured as its own top-level project so that its CRT "
    "setting cannot be overridden by the EpoCanva root project.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT MSVC)
  message(FATAL_ERROR
    "The P1.2/P1.3 baseline is a Windows/MSVC artefact; this project links its "
    "import library.")
endif()

if(NOT CMAKE_GENERATOR_PLATFORM STREQUAL "x64")
  message(WARNING
    "CMAKE_GENERATOR_PLATFORM is '${CMAKE_GENERATOR_PLATFORM}', not x64. The "
    "baseline librime build is x64 and rime.lib will not link against anything else.")
endif()

# Must match the CRT of rime.lib and of every static dependency folded into it.
# Declared here rather than on the command line so a stale cache cannot quietly
# leave it at the default /MD. CMP0091 is NEW at cmake_minimum_required 3.25, so
# this variable -- not a make-rules override -- is what selects the switch.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

set(Rime_Install_Prefix "" CACHE PATH
  "Installation prefix produced by tools/build_librime_baseline.ps1 (the directory holding bin/, lib/, include/)")
if(NOT Rime_Install_Prefix)
  message(FATAL_ERROR
    "Rime_Install_Prefix is required. Point it at build/librime-baseline/rime/install.")
endif()
get_filename_component(Rime_Install_Prefix "${Rime_Install_Prefix}" ABSOLUTE)
if(NOT IS_DIRECTORY "${Rime_Install_Prefix}/include")
  message(FATAL_ERROR "no include/ under Rime_Install_Prefix: ${Rime_Install_Prefix}")
endif()

# Upstream installs the Windows DLL next to its import library (lib/) while the
# rime_*.exe tools go to bin/; search both, but never outside the given prefix.
find_file(Rime_Api_Header rime_api.h
  PATHS "${Rime_Install_Prefix}/include"
  NO_DEFAULT_PATH
  REQUIRED)
find_library(Rime_Import_Library rime
  PATHS "${Rime_Install_Prefix}/lib"
  NO_DEFAULT_PATH
  REQUIRED)
find_file(Rime_Shared_Library rime.dll
  PATHS "${Rime_Install_Prefix}/lib" "${Rime_Install_Prefix}/bin"
  NO_DEFAULT_PATH
  REQUIRED)

# NO_DEFAULT_PATH is what keeps this project from silently linking a librime
# installed elsewhere on the machine.
file(TO_CMAKE_PATH "${Rime_Install_Prefix}" _norm_prefix)
foreach(_found Rime_Api_Header Rime_Import_Library Rime_Shared_Library)
  file(TO_CMAKE_PATH "${${_found}}" _norm_found)
  string(FIND "${_norm_found}" "${_norm_prefix}/" _prefix_offset)
  if(NOT _prefix_offset EQUAL 0)
    message(FATAL_ERROR
      "${_found} resolved to ${${_found}}, which is not under ${Rime_Install_Prefix}")
  endif()
  message(STATUS "  ${_found} = ${${_found}}")
endforeach()

# The test fixture is versioned input, not build output. Resolved relative to this
# project file so a clean checkout on another machine configures without editing
# anything; a missing fixture fails here rather than at run time as a mystery.
set(EpoCanva_Rime_Raw_Fixture_Dir
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fixtures/rime_raw" CACHE PATH
  "Directory holding default.yaml and epocanva_test_raw.schema.yaml for the replay test")
get_filename_component(EpoCanva_Rime_Raw_Fixture_Dir
  "${EpoCanva_Rime_Raw_Fixture_Dir}" ABSOLUTE)
foreach(_fixture default.yaml epocanva_test_raw.schema.yaml)
  if(NOT EXISTS "${EpoCanva_Rime_Raw_Fixture_Dir}/${_fixture}")
    message(FATAL_ERROR
      "test fixture missing: ${EpoCanva_Rime_Raw_Fixture_Dir}/${_fixture}")
  endif()
endforeach()

add_executable(rime_input_replay main.cc)
target_compile_definitions(rime_input_replay PRIVATE
  RIME_IMPORTS            # rime_api.h:23-26 -- dllimport view of the API
  _CRT_SECURE_NO_WARNINGS
  NOMINMAX
  WIN32_LEAN_AND_MEAN)
# Same strictness the root EpoCanva project and the lifecycle probe apply.
target_compile_options(rime_input_replay PRIVATE
  /utf-8 /permissive- /Zc:__cplusplus /Zc:preprocessor /W4 /EHsc)
target_include_directories(rime_input_replay PRIVATE
  "${Rime_Install_Prefix}/include")
target_link_libraries(rime_input_replay PRIVATE "${Rime_Import_Library}")

# Windows resolves an implicitly loaded DLL from the executable's own directory
# first, so placing the exact built rime.dll there makes the load path explicit and
# independent of PATH. The program then verifies at run time that the module which
# supplied rime_get_api() really is this file.
add_custom_command(TARGET rime_input_replay POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${Rime_Shared_Library}"
    "$<TARGET_FILE_DIR:rime_input_replay>/rime.dll"
  COMMENT "Placing the built rime.dll next to the input replay test")

enable_testing()

# Every run root sits under one directory named rime-input-replay-run. The replay
# program will only delete a path whose parent has exactly that name, so a wrong
# --run-root on the command line cannot make it clean out something else.
set(_replay_run_root "${CMAKE_CURRENT_BINARY_DIR}/rime-input-replay-run")

add_test(
  NAME librime.input_replay.raw_schema_key_event_matrix
  COMMAND rime_input_replay
    --expect-dll "$<TARGET_FILE_DIR:rime_input_replay>/rime.dll"
    --fixture-dir "${EpoCanva_Rime_Raw_Fixture_Dir}"
    --run-root "${_replay_run_root}/matrix"
)

add_test(
  NAME librime.input_replay.raw_schema_missing_fixture_fails_closed
  COMMAND rime_input_replay
    --expect-dll "$<TARGET_FILE_DIR:rime_input_replay>/rime.dll"
    --fixture-dir "${EpoCanva_Rime_Raw_Fixture_Dir}"
    --run-root "${_replay_run_root}/missing-fixture"
    --expect-missing-fixture
)

# Exit status alone decides the result. Neither PASS_REGULAR_EXPRESSION nor
# WILL_FAIL is used: both can turn a non-zero exit into a green test, and the
# missing-fixture case is a positive assertion about an absence rather than an
# inverted expectation, so it needs no WILL_FAIL.
set_tests_properties(librime.input_replay.raw_schema_key_event_matrix
  PROPERTIES
    TIMEOUT 120
    LABELS "librime;input_replay;matrix")

set_tests_properties(librime.input_replay.raw_schema_missing_fixture_fails_closed
  PROPERTIES
    TIMEOUT 120
    LABELS "librime;input_replay;negative")

message(STATUS "epocanva_rime_input_replay summary")
message(STATUS "  generator              : ${CMAKE_GENERATOR}")
message(STATUS "  generator platform     : ${CMAKE_GENERATOR_PLATFORM}")
message(STATUS "  configuration types    : ${CMAKE_CONFIGURATION_TYPES}")
message(STATUS "  MSVC runtime library   : ${CMAKE_MSVC_RUNTIME_LIBRARY}")
message(STATUS "  rime install prefix    : ${Rime_Install_Prefix}")
message(STATUS "  fixture directory      : ${EpoCanva_Rime_Raw_Fixture_Dir}")
message(STATUS "  run roots              : ${_replay_run_root}/{matrix,missing-fixture}")
