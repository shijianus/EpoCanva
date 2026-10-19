# EpoCanva P2.6-recovery -- standalone project for the rebuilt English integration tests.
#
# Deliberately NOT add_subdirectory()'d into the EpoCanva root project, for the reason
# recorded in P1.3 and re-asserted by the original tools/rime_english_completion: the root
# project is a Debug / dynamic-CRT build while these programs must link the Release /
# static-CRT rime.lib. Keeping them apart is what stops the two CRTs leaking into each
# other, so this project refuses to be configured as anything but its own top level.
#
# Two executables are built from the SAME sources on purpose:
#
#   rime_english_recovery          links the ENGLISH build's rime.lib and runs with the
#                                  english-built rime.dll next to it
#   rime_english_recovery_baseline links the unmodified BASELINE rime.lib and runs with
#                                  the baseline rime.dll next to it
#
# Each lives in its own output directory because Windows resolves an implicitly loaded
# DLL from the executable's own directory first, and only one file can be named rime.dll
# there. Every program also verifies at run time which module actually supplied
# rime_get_api(), so the cross-injection cases below are not taken on trust.
#
# WHY THIS PROJECT EXISTS ALONGSIDE tools/rime_english_completion. The original
# integration harness was one ~5,100 line main.cc and that file is gone (truncated on
# 2026-10-19; it is committed empty in the snapshot so the loss is visible). This is a
# modular rebuild of it: entry, shared harness, and one file per behaviour group. The old
# project and its CTest registrations are left untouched and remain the record of what
# still has to be recovered -- nothing here deletes them.
#
# Configure with:
#   cmake -S tools/rime_english_recovery -B build/english-recovery/test `
#         -G "Visual Studio 17 2022" -A x64 -DCMAKE_CONFIGURATION_TYPES:STRING=Release `
#         -DRime_Install_Prefix=<repo>/build/librime-english/rime/install `
#         -DBaseline_Rime_Prefix=<repo>/build/librime-baseline/rime/install

cmake_minimum_required(VERSION 3.25)
project(epocanva_rime_english_recovery VERSION 0.1.0 LANGUAGES CXX)

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
    "The librime artefacts this project links against are Windows/MSVC builds.")
endif()

if(NOT CMAKE_GENERATOR_PLATFORM STREQUAL "x64")
  message(WARNING
    "CMAKE_GENERATOR_PLATFORM is '${CMAKE_GENERATOR_PLATFORM}', not x64. Neither "
    "rime.lib will link against anything else.")
endif()

# Must match the CRT of rime.lib and of every static dependency folded into it.
# Declared here rather than on the command line so a stale cache cannot quietly leave it
# at the default /MD. CMP0091 is NEW at cmake_minimum_required 3.25.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

set(Rime_Install_Prefix "" CACHE PATH
  "Installation prefix of the ENGLISH build (build/librime-english/rime/install)")
set(Baseline_Rime_Prefix "" CACHE PATH
  "Installation prefix of the unmodified baseline build")
set(EpoCanva_English_Header_Dir
    "${CMAKE_CURRENT_SOURCE_DIR}/../../engine/rime_components/english" CACHE PATH
  "Directory holding english_diagnostics.h, the C surface of the component")
set(EpoCanva_English_Fixture_Dir
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fixtures/english_completion" CACHE PATH
  "Directory holding default.yaml, the schema and the word lists")
set(EpoCanva_Assert_Script
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/english_completion/assert_exit.cmake"
    CACHE FILEPATH "Wrapper that compares an exact exit code and a marker policy")

# Reuses the surviving wrapper rather than inventing a second judgement: exact integer
# exit code, plus the success marker required or forbidden. The engine half of every
# claim is the program's own observation, not this file's.

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
  # elsewhere on the machine; the loop additionally proves each hit is inside the prefix
  # it was asked for.
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

epocanva_locate_engine(ENGLISH  Rime_Install_Prefix  Eng)
epocanva_locate_engine(BASELINE Baseline_Rime_Prefix Base)

# --- versioned inputs must exist, or the build fails at configure time ----------
foreach(_f
        "${EpoCanva_English_Header_Dir}/english_diagnostics.h"
        "${EpoCanva_English_Fixture_Dir}/default.yaml"
        "${EpoCanva_English_Fixture_Dir}/epocanva_test_english.schema.yaml"
        "${EpoCanva_English_Fixture_Dir}/lexicon.tsv"
        "${EpoCanva_English_Fixture_Dir}/README.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/main.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/recovery_test_support.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/recovery_test_support.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/completion_tests.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/completion_tests.cc"
        "${EpoCanva_Assert_Script}")
  if(NOT EXISTS "${_f}")
    message(FATAL_ERROR "required input missing: ${_f}")
  endif()
endforeach()

set(_SOURCES
  main.cc
  recovery_test_support.cc
  completion_tests.cc)
set(_DEFINES RIME_IMPORTS _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
set(_OPTIONS /utf-8 /permissive- /Zc:__cplusplus /W4 /EHsc)

# RIME_IMPORTS is the dllimport view of the API (rime_api.h:23-26); /W4 is kept because
# this project's whole purpose is observing things precisely.
add_executable(rime_english_recovery ${_SOURCES})
target_compile_definitions(rime_english_recovery PRIVATE ${_DEFINES})
target_compile_options(rime_english_recovery PRIVATE ${_OPTIONS})
target_include_directories(rime_english_recovery PRIVATE
  "${Eng_INCLUDE_DIR}" "${EpoCanva_English_Header_Dir}")
target_link_libraries(rime_english_recovery PRIVATE "${Eng_IMPORT_LIB}")
set_target_properties(rime_english_recovery PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_BINARY_DIR}/english-engine")

add_executable(rime_english_recovery_baseline ${_SOURCES})
target_compile_definitions(rime_english_recovery_baseline PRIVATE ${_DEFINES})
target_compile_options(rime_english_recovery_baseline PRIVATE ${_OPTIONS})
target_include_directories(rime_english_recovery_baseline PRIVATE
  "${Base_INCLUDE_DIR}" "${EpoCanva_English_Header_Dir}")
target_link_libraries(rime_english_recovery_baseline PRIVATE "${Base_IMPORT_LIB}")
set_target_properties(rime_english_recovery_baseline PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_BINARY_DIR}/baseline-engine")

# Place the exact built rime.dll in each executable's own directory, so the load path is
# explicit and independent of PATH. Each program then verifies at run time that the module
# which supplied rime_get_api() really is that file.
add_custom_command(TARGET rime_english_recovery POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${Eng_SHARED_LIB}"
    "$<TARGET_FILE_DIR:rime_english_recovery>/rime.dll"
  COMMENT "Placing the english-built rime.dll next to the recovery test")
add_custom_command(TARGET rime_english_recovery_baseline POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${Base_SHARED_LIB}"
    "$<TARGET_FILE_DIR:rime_english_recovery_baseline>/rime.dll"
  COMMENT "Placing the unmodified baseline rime.dll next to the recovery test")

enable_testing()

# Every run root sits under one directory named rime-english-recovery-run; the program
# deletes a path only when its parent has exactly that name.
set(_run_root "${CMAKE_CURRENT_BINARY_DIR}/rime-english-recovery-run")
set(_assert ${EpoCanva_Assert_Script})

function(epocanva_add_case)
  cmake_parse_arguments(CASE "BASELINE_EXE"
                        "NAME;MODE;EXPECTED_EXIT;MARKER;MARKER_POLICY;LABEL"
                        "" ${ARGN})
  if(NOT CASE_MARKER_POLICY)
    set(CASE_MARKER_POLICY require)
  endif()
  if(CASE_BASELINE_EXE)
    set(exe_dir "${CMAKE_CURRENT_BINARY_DIR}/baseline-engine")
    set(exe "$<TARGET_FILE:rime_english_recovery_baseline>")
  else()
    set(exe_dir "${CMAKE_CURRENT_BINARY_DIR}/english-engine")
    set(exe "$<TARGET_FILE:rime_english_recovery>")
  endif()
  add_test(
    NAME ${CASE_NAME}
    COMMAND ${CMAKE_COMMAND}
      -DEXE=${exe}
      -DCASE=${CASE_LABEL}
      -DEXPECTED_EXIT=${CASE_EXPECTED_EXIT}
      "-DMARKER=${CASE_MARKER}"
      "-DMARKER_POLICY=${CASE_MARKER_POLICY}"
      "-DPASS_ARGS=--expect-dll;${exe_dir}/rime.dll;--fixture-dir;${EpoCanva_English_Fixture_Dir};--run-root;${_run_root}/${CASE_LABEL};--mode;${CASE_MODE}"
      -DTIMEOUT_SECONDS=120
      -P ${_assert}
  )
endfunction()

# 1. The positive run. Real keystrokes, real menu, real commit queue, exit 0 and the
#    PASS marker.
epocanva_add_case(
  NAME      recovery.completion.prefix_candidates_and_commit
  MODE      completion
  EXPECTED_EXIT 0
  MARKER    "RESULT PASS"
  MARKER_POLICY require
  LABEL     completion)

# 2. The unmodified baseline engine must show no module and no candidates AT ALL, while
#    its composition and its commit still work.
epocanva_add_case(
  NAME      recovery.completion.negative_baseline_engine
  BASELINE_EXE TRUE
  MODE      expect-module-absent
  EXPECTED_EXIT 0
  MARKER    "RESULT PASS"
  MARKER_POLICY require
  LABEL     expect-module-absent)

# 3. THE POSITIVE TEST MUST FAIL ON THE WRONG ENGINE. Same program, same mode, the
#    baseline rime.dll: the module is absent, so it must exit 20. Without this case a
#    green run of case 1 would not prove the assertions are wired to the engine binary
#    rather than to the --mode string.
epocanva_add_case(
  NAME      recovery.completion.positive_mode_fails_on_baseline_engine
  BASELINE_EXE TRUE
  MODE      completion
  EXPECTED_EXIT 20
  MARKER    "RESULT PASS"
  MARKER_POLICY forbid
  LABEL     completion-on-baseline)

# 4. THE NEGATIVE TEST MUST FAIL ON THE WRONG ENGINE. The english-linked program asked to
#    assert an absence finds the module present, so it must exit 18.
epocanva_add_case(
  NAME      recovery.completion.negative_mode_fails_on_english_engine
  MODE      expect-module-absent
  EXPECTED_EXIT 18
  MARKER    "RESULT PASS"
  MARKER_POLICY forbid
  LABEL     expect-module-absent-on-english)

# 5. Bad arguments are refused rather than ignored, and an unimplemented mode name cannot
#    reach a dispatch.
epocanva_add_case(
  NAME      recovery.completion.rejects_unknown_mode
  MODE      not-a-real-mode
  EXPECTED_EXIT 2
  MARKER    "RESULT PASS"
  MARKER_POLICY forbid
  LABEL     unknown-mode)

# 6. A test that cannot fail is not a test (AGENTS.md section 3). This run types a real
#    composition and asserts a menu the fixture provably does not contain, so it must
#    report an assertion failure -- exit 14 -- and must not print its marker.
epocanva_add_case(
  NAME      recovery.completion.suite_can_fail
  MODE      self-check-fail
  EXPECTED_EXIT 14
  MARKER    "RESULT PASS"
  MARKER_POLICY forbid
  LABEL     self-check-fail)

set_tests_properties(
  recovery.completion.prefix_candidates_and_commit
  recovery.completion.negative_baseline_engine
  recovery.completion.positive_mode_fails_on_baseline_engine
  recovery.completion.negative_mode_fails_on_english_engine
  recovery.completion.rejects_unknown_mode
  recovery.completion.suite_can_fail
  PROPERTIES TIMEOUT 180 LABELS "librime;english;completion;recovery")

# The wrapper's own timeout is one notch below the CTest TIMEOUT so a hung program is
# reported by the wrapper (non-numeric result) before CTest kills the run.

message(STATUS "epocanva_rime_english_recovery summary")
message(STATUS "  generator            : ${CMAKE_GENERATOR}")
message(STATUS "  configuration types  : ${CMAKE_CONFIGURATION_TYPES}")
message(STATUS "  MSVC runtime library : ${CMAKE_MSVC_RUNTIME_LIBRARY}")
message(STATUS "  english prefix       : ${Rime_Install_Prefix}")
message(STATUS "  baseline prefix      : ${Baseline_Rime_Prefix}")
message(STATUS "  diagnostics header   : ${EpoCanva_English_Header_Dir}")
message(STATUS "  fixture dir          : ${EpoCanva_English_Fixture_Dir}")
message(STATUS "  assert wrapper       : ${EpoCanva_Assert_Script}")
message(STATUS "  run root             : ${_run_root}")
