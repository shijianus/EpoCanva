# EpoCanva P1.4 -- standalone project for the compiled-in component probe.
#
# Deliberately NOT add_subdirectory()'d into the root EpoCanva project, for the
# same reason as tools/rime_input_replay: the root project is a Debug /
# dynamic-CRT toolchain probe, while these programs must link the Release /
# static-CRT librime import libraries. Separate projects keep the two CRTs from
# leaking into each other.
#
# Two executables are built on purpose, from one source file:
#
#   rime_component_probe             links the COMPONENT build's rime.lib and runs
#                                    with the component build's rime.dll next to it
#   rime_component_probe_baseline    links the BASELINE build's rime.lib and runs
#                                    with the unmodified rime.dll next to it
#
# Each lives in its own output directory because Windows resolves an implicitly
# loaded DLL from the executable's own directory first, and only one file can be
# named rime.dll there. Two processes, two engines, no PATH involvement, and the
# program still verifies at run time which module supplied rime_get_api().
#
# Configure with:
#   cmake -S tools/rime_component_probe \
#         -B build/librime-component-probe/probe \
#         -G "Visual Studio 17 2022" -A x64 \
#         -DCMAKE_CONFIGURATION_TYPES:STRING=Release \
#         -DRime_Install_Prefix=<repo>/build/librime-component-probe/rime/install \
#         -DBaseline_Rime_Prefix=<repo>/build/librime-baseline/rime/install

cmake_minimum_required(VERSION 3.25)
project(epocanva_rime_component_probe VERSION 0.1.0 LANGUAGES CXX)

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
    "The P1.2/P1.4 librime artefacts are Windows/MSVC builds; these programs link "
    "their import libraries.")
endif()

if(NOT CMAKE_GENERATOR_PLATFORM STREQUAL "x64")
  message(WARNING
    "CMAKE_GENERATOR_PLATFORM is '${CMAKE_GENERATOR_PLATFORM}', not x64. Neither "
    "rime.lib will link against anything else.")
endif()

# Must match the CRT of rime.lib and of every static dependency folded into it.
# Declared here rather than on the command line so a stale cache cannot quietly
# leave it at the default /MD. CMP0091 is NEW at cmake_minimum_required 3.25.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

set(Rime_Install_Prefix "" CACHE PATH
  "Installation prefix of the COMPONENT build (build/librime-component-probe/rime/install)")
set(Baseline_Rime_Prefix "" CACHE PATH
  "Installation prefix of the unmodified baseline build (build/librime-baseline/rime/install)")
set(EpoCanva_Probe_Header_Dir
    "${CMAKE_CURRENT_SOURCE_DIR}/../../engine/rime_components/probe" CACHE PATH
  "Directory holding probe_diagnostics.h, the C surface of the compiled-in component")
set(EpoCanva_Rime_Probe_Fixture_Dir
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fixtures/rime_probe" CACHE PATH
  "Directory holding default.yaml and epocanva_test_probe.schema.yaml")
set(EpoCanva_Rime_Raw_Fixture_Dir
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fixtures/rime_raw" CACHE PATH
  "Directory holding the P1.3 raw fixture, re-run against the component engine")
set(EpoCanva_Rime_Input_Replay_Exe "" CACHE FILEPATH
  "Path to the already built rime_input_replay.exe (release regression on the component engine)")

function(epocanva_locate_engine _label _prefix _out_var)
  if(NOT ${_prefix})
    message(FATAL_ERROR "${_label}: the ${_out_var} cache variable is required.")
  endif()
  get_filename_component(_abs "${${_prefix}}" ABSOLUTE)
  if(NOT IS_DIRECTORY "${_abs}/include")
    message(FATAL_ERROR "${_label}: no include/ under ${_abs}")
  endif()
  find_file(_hdr${_label} rime_api.h PATHS "${_abs}/include" NO_DEFAULT_PATH REQUIRED)
  find_library(_lib${_label} rime PATHS "${_abs}/lib" NO_DEFAULT_PATH REQUIRED)
  find_file(_dll${_label} rime.dll PATHS "${_abs}/lib" "${_abs}/bin"
            NO_DEFAULT_PATH REQUIRED)
  # NO_DEFAULT_PATH keeps this project from silently resolving a librime installed
  # elsewhere on the machine; the loop below additionally proves the found path is
  # inside the prefix it was asked for.
  file(TO_CMAKE_PATH "${_abs}" _norm_prefix)
  foreach(_v _hdr${_label} _lib${_label} _dll${_label})
    file(TO_CMAKE_PATH "${${_v}}" _norm_found)
    string(FIND "${_norm_found}" "${_norm_prefix}/" _off)
    if(NOT _off EQUAL 0)
      message(FATAL_ERROR
        "${_label}: ${_v} resolved to ${${_v}}, which is not under ${_abs}")
    endif()
    message(STATUS "  ${_label} ${_v} = ${${_v}}")
  endforeach()
  set(${_out_var}_INCLUDE_DIR "${_abs}/include" PARENT_SCOPE)
  set(${_out_var}_IMPORT_LIB "${_lib${_label}}" PARENT_SCOPE)
  set(${_out_var}_SHARED_LIB "${_dll${_label}}" PARENT_SCOPE)
endfunction()

epocanva_locate_engine(PROBE      Rime_Install_Prefix     Comp)
epocanva_locate_engine(BASELINE  Baseline_Rime_Prefix    Base)

# --- versioned inputs must exist, or the build fails at configure time ----------
foreach(_f
        "${EpoCanva_Probe_Header_Dir}/probe_diagnostics.h"
        "${EpoCanva_Probe_Header_Dir}/probe_processor.h"
        "${EpoCanva_Probe_Header_Dir}/probe_processor.cc"
        "${EpoCanva_Probe_Header_Dir}/probe_module.cc"
        "${EpoCanva_Probe_Header_Dir}/CMakeLists.txt"
        "${EpoCanva_Rime_Probe_Fixture_Dir}/default.yaml"
        "${EpoCanva_Rime_Probe_Fixture_Dir}/epocanva_test_probe.schema.yaml"
        "${EpoCanva_Rime_Probe_Fixture_Dir}/README.md")
  if(NOT EXISTS "${_f}")
    message(FATAL_ERROR "required input missing: ${_f}")
  endif()
endforeach()

# The component source and the test program must agree on one C header; the
# program includes it from the component's own directory, not from a copy, so the
# layout it asserts is the layout the engine was compiled against.
add_executable(rime_component_probe main.cc)
target_compile_definitions(rime_component_probe PRIVATE
  RIME_IMPORTS            # rime_api.h:23-26 -- dllimport view of the API
  _CRT_SECURE_NO_WARNINGS
  NOMINMAX
  WIN32_LEAN_AND_MEAN)
target_compile_options(rime_component_probe PRIVATE
  /utf-8 /permissive- /Zc:__cplusplus /Zc:preprocessor /W4 /EHsc)
target_include_directories(rime_component_probe PRIVATE
  "${Comp_INCLUDE_DIR}" "${EpoCanva_Probe_Header_Dir}")
target_link_libraries(rime_component_probe PRIVATE "${Comp_IMPORT_LIB}")
set_target_properties(rime_component_probe PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_BINARY_DIR}/component-engine")

add_executable(rime_component_probe_baseline main.cc)
target_compile_definitions(rime_component_probe_baseline PRIVATE
  RIME_IMPORTS _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(rime_component_probe_baseline PRIVATE
  /utf-8 /permissive- /Zc:__cplusplus /Zc:preprocessor /W4 /EHsc)
target_include_directories(rime_component_probe_baseline PRIVATE
  "${Base_INCLUDE_DIR}" "${EpoCanva_Probe_Header_Dir}")
target_link_libraries(rime_component_probe_baseline PRIVATE "${Base_IMPORT_LIB}")
set_target_properties(rime_component_probe_baseline PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_BINARY_DIR}/baseline-engine")

# Place the exact built rime.dll in each executable's own directory, so the load
# path is explicit and independent of PATH. Each program then verifies at run time
# that the module which supplied rime_get_api() really is that file.
add_custom_command(TARGET rime_component_probe POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${Comp_SHARED_LIB}"
    "$<TARGET_FILE_DIR:rime_component_probe>/rime.dll"
  COMMENT "Placing the component-built rime.dll next to the probe test")
add_custom_command(TARGET rime_component_probe_baseline POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${Base_SHARED_LIB}"
    "$<TARGET_FILE_DIR:rime_component_probe_baseline>/rime.dll"
  COMMENT "Placing the unmodified baseline rime.dll next to the negative test")

# --- release regression: the P1.3 replay program against the COMPONENT engine -----
# A copy of the already built rime_input_replay.exe is placed next to the component
# rime.dll in its own directory, so the unmodified baseline tree is never touched
# and the two engines stay separate processes.
set(_replay_dir "${CMAKE_CURRENT_BINARY_DIR}/replay-regression")
if(EpoCanva_Rime_Input_Replay_Exe)
  get_filename_component(_replay_exe "${EpoCanva_Rime_Input_Replay_Exe}" ABSOLUTE)
  if(NOT EXISTS "${_replay_exe}")
    message(FATAL_ERROR
      "EpoCanva_Rime_Input_Replay_Exe does not exist: ${_replay_exe}. Build "
      "tools/rime_input_replay first, or leave the variable empty to skip the "
      "replay regression.")
  endif()
  add_custom_target(epocanva_replay_regression_payload ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_replay_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${_replay_exe}" "${_replay_dir}/rime_input_replay.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${Comp_SHARED_LIB}" "${_replay_dir}/rime.dll"
    COMMENT "Staging rime_input_replay.exe against the component-built rime.dll")
  add_dependencies(epocanva_replay_regression_payload rime_component_probe)
else()
  message(STATUS
    "EpoCanva_Rime_Input_Replay_Exe not set: the replay regression on the "
    "component engine will not be registered as a test.")
endif()

enable_testing()

# Every run root sits under one directory named rime-component-probe-run; the
# program will only delete a path whose parent has exactly that name.
set(_run_root "${CMAKE_CURRENT_BINARY_DIR}/rime-component-probe-run")

add_test(
  NAME librime.component_probe.compiled_in_module_and_processor
  COMMAND rime_component_probe
    --expect-dll "$<TARGET_FILE_DIR:rime_component_probe>/rime.dll"
    --fixture-dir "${EpoCanva_Rime_Probe_Fixture_Dir}"
    --run-root "${_run_root}/probe"
    --mode probe
)

add_test(
  NAME librime.component_probe.negative_baseline_engine
  COMMAND rime_component_probe_baseline
    --expect-dll "$<TARGET_FILE_DIR:rime_component_probe_baseline>/rime.dll"
    --fixture-dir "${EpoCanva_Rime_Probe_Fixture_Dir}"
    --run-root "${_run_root}/negative-baseline"
    --mode expect-module-absent
)

add_test(
  NAME librime.component_probe.negative_schema_without_component
  COMMAND rime_component_probe
    --expect-dll "$<TARGET_FILE_DIR:rime_component_probe>/rime.dll"
    --fixture-dir "${EpoCanva_Rime_Probe_Fixture_Dir}"
    --run-root "${_run_root}/negative-schema"
    --mode expect-component-absent
)

set_tests_properties(librime.component_probe.compiled_in_module_and_processor
  PROPERTIES TIMEOUT 180 LABELS "librime;component_probe;matrix")
set_tests_properties(librime.component_probe.negative_baseline_engine
  PROPERTIES TIMEOUT 180 LABELS "librime;component_probe;negative")
set_tests_properties(librime.component_probe.negative_schema_without_component
  PROPERTIES TIMEOUT 180 LABELS "librime;component_probe;negative")

# Exit status alone decides each result. No PASS_REGULAR_EXPRESSION, no WILL_FAIL:
# the two negative tests are positive assertions about an absence and must
# themselves return 0.
if(EpoCanva_Rime_Input_Replay_Exe)
  add_test(
    NAME librime.component_probe.replay_regression_on_component_engine
    COMMAND "${_replay_dir}/rime_input_replay.exe"
      --expect-dll "${_replay_dir}/rime.dll"
      --fixture-dir "${EpoCanva_Rime_Raw_Fixture_Dir}"
      --run-root "${_replay_dir}/rime-input-replay-run/matrix"
  )
  # epocanva_replay_regression_payload is an ALL target, so `cmake --build` stages
  # the exe and the DLL before ctest ever runs.
  set_tests_properties(librime.component_probe.replay_regression_on_component_engine
    PROPERTIES TIMEOUT 180 LABELS "librime;component_probe;regression")

  add_test(
    NAME librime.component_probe.replay_missing_fixture_on_component_engine
    COMMAND "${_replay_dir}/rime_input_replay.exe"
      --expect-dll "${_replay_dir}/rime.dll"
      --fixture-dir "${EpoCanva_Rime_Raw_Fixture_Dir}"
      --run-root "${_replay_dir}/rime-input-replay-run/missing-fixture"
      --expect-missing-fixture
  )
  set_tests_properties(librime.component_probe.replay_missing_fixture_on_component_engine
    PROPERTIES TIMEOUT 180 LABELS "librime;component_probe;regression;negative")
endif()

message(STATUS "epocanva_rime_component_probe summary")
message(STATUS "  generator              : ${CMAKE_GENERATOR}")
message(STATUS "  generator platform     : ${CMAKE_GENERATOR_PLATFORM}")
message(STATUS "  configuration types    : ${CMAKE_CONFIGURATION_TYPES}")
message(STATUS "  MSVC runtime library   : ${CMAKE_MSVC_RUNTIME_LIBRARY}")
message(STATUS "  component prefix       : ${Rime_Install_Prefix}")
message(STATUS "  baseline prefix        : ${Baseline_Rime_Prefix}")
message(STATUS "  diagnostics header dir: ${EpoCanva_Probe_Header_Dir}")
message(STATUS "  probe fixture dir      : ${EpoCanva_Rime_Probe_Fixture_Dir}")
message(STATUS "  replay regression exe  : ${EpoCanva_Rime_Input_Replay_Exe}")
message(STATUS "  run root               : ${_run_root}")
