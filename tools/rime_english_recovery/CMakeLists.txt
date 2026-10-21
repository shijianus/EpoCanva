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
# A THIRD executable, rime_english_recovery_sha256, is built from the digest helper plus its own
# test file and links NEITHER rime.lib. pilot_sha256.cc is what the pilot data identity rests on,
# so it gets cases that need no engine and cannot be excused by engine setup. That target is also
# where bcrypt.lib is allowed to go: into the three test executables that compile pilot_sha256.cc
# and nowhere else -- rime.dll, the engine and engine/languages/en keep no dependency on it. The
# helper calls Windows CNG; this project makes no cross-platform claim about it.
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
# The P2.6 spelling suite runs on its OWN fixture directory, so that enabling suggestions
# cannot change what any other case measures. Only these three files live here; the broken
# schema and its three-row word list are derived by the program at staging time, because the
# difference from the original is itself the assertion
# (tests/fixtures/english_spelling/README.md section 8).
set(EpoCanva_English_Spelling_Fixture_Dir
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fixtures/english_spelling" CACHE PATH
  "Directory holding the spelling fixture's default.yaml, schema and 16-row word list")

# --- the pilot switch: OFF by default, and OFF means the data is never mentioned -------
# EPOCANVA_ENGLISH_PILOT_LEXICON is empty in the normal configure, and with it empty this
# project registers exactly the cases it registered before the pilot existed, requires no
# generated data, and reads no manifest. Setting it is a promise that the data is present
# and is the pinned data: every check below runs at configure time and fails the configure
# loudly rather than deregistering the case, so there is no such thing as a machine that
# "passed" the pilot suite by not running it. Nothing here downloads, fetches or builds the
# data; tools/data/build_english_pilot.py is run by a human, and the option only names the
# result. See data/manifests/english-pilot.sources.lock.json for what the file must be.
set(EPOCANVA_ENGLISH_PILOT_LEXICON "" CACHE FILEPATH
  "Absolute path of the generated pilot word list; empty registers no pilot case")
set(EPOCANVA_ENGLISH_PILOT_OUTPUT_DIR "" CACHE PATH
  "Directory holding the generated pilot lexicon and its conversion report")
set(EpoCanva_English_Pilot_Fixture_Dir
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fixtures/english_pilot" CACHE PATH
  "Directory holding the pilot default.yaml and schema; used only by pilot cases")
set(EpoCanva_Pilot_Manifest
    "${CMAKE_CURRENT_SOURCE_DIR}/../../data/manifests/english-pilot.sources.lock.json"
    CACHE FILEPATH "The pinned identity record the pilot data is checked against")

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
        "${EpoCanva_English_Fixture_Dir}/lexicon_bad_duplicate.tsv"
        "${EpoCanva_English_Fixture_Dir}/lexicon_bad_illegal.tsv"
        "${EpoCanva_English_Fixture_Dir}/lexicon_bad_illegal_form.tsv"
        "${EpoCanva_English_Fixture_Dir}/README.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/main.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/recovery_test_support.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/recovery_test_support.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/completion_tests.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/completion_tests.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/case_tests.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/case_tests.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/token_tests.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/token_tests.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/space_tests.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/space_tests.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/lexicon_failure_tests.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/lexicon_failure_tests.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/spelling_tests.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/spelling_tests.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/spelling_commit_tests.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/spelling_commit_tests.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/spelling_negative_tests.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/spelling_negative_tests.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fixtures/english_spelling/default.yaml"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fixtures/english_spelling/epocanva_spell_english.schema.yaml"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fixtures/english_spelling/lexicon_spell.tsv"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fixtures/english_spelling/README.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/pilot_sha256.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/pilot_sha256.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/pilot_sha256_tests.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/pilot_reference.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/pilot_reference.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/pilot_tests.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/pilot_tests.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fixtures/english_pilot/default.yaml"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fixtures/english_pilot/epocanva_pilot_english.schema.yaml"
        "${EpoCanva_Assert_Script}")
  if(NOT EXISTS "${_f}")
    message(FATAL_ERROR "required input missing: ${_f}")
  endif()
endforeach()

# --- pilot identity, established before anything is compiled -------------------------
# The expectations the test program carries are read OUT of the manifest, and the file on
# disk is hashed against them here. Both directions matter: a wrong artefact cannot be
# accommodated by editing this file (the digest comes from the manifest, not from a literal
# below), and an edited manifest cannot be accommodated by the artefact (the hash of the
# real file is compared, and a mismatch stops the configure). A truncated list is the case
# that shows why: it still parses and still loads, so only the digest separates it.
if(EPOCANVA_ENGLISH_PILOT_LEXICON)
  if(NOT IS_ABSOLUTE "${EPOCANVA_ENGLISH_PILOT_LEXICON}")
    message(FATAL_ERROR
      "EPOCANVA_ENGLISH_PILOT_LEXICON must be an absolute path, got: "
      "${EPOCANVA_ENGLISH_PILOT_LEXICON}")
  endif()
  if(NOT EXISTS "${EPOCANVA_ENGLISH_PILOT_LEXICON}")
    message(FATAL_ERROR
      "EPOCANVA_ENGLISH_PILOT_LEXICON names a file that does not exist: "
      "${EPOCANVA_ENGLISH_PILOT_LEXICON}. Generate it with "
      "tools/data/build_english_pilot.py (no network step is run by this build).")
  endif()
  if(NOT EPOCANVA_ENGLISH_PILOT_OUTPUT_DIR)
    get_filename_component(EPOCANVA_ENGLISH_PILOT_OUTPUT_DIR
      "${EPOCANVA_ENGLISH_PILOT_LEXICON}" DIRECTORY)
    set(EPOCANVA_ENGLISH_PILOT_OUTPUT_DIR "${EPOCANVA_ENGLISH_PILOT_OUTPUT_DIR}" CACHE PATH
      "Directory holding the generated pilot lexicon and its conversion report" FORCE)
  endif()
  if(NOT EXISTS "${EpoCanva_Pilot_Manifest}")
    message(FATAL_ERROR
      "the pilot manifest is missing, so no identity expectation can be read: "
      "${EpoCanva_Pilot_Manifest}")
  endif()
  if(NOT EXISTS "${EpoCanva_English_Pilot_Fixture_Dir}/epocanva_pilot_english.schema.yaml")
    message(FATAL_ERROR
      "the pilot fixture directory is missing its schema: "
      "${EpoCanva_English_Pilot_Fixture_Dir}")
  endif()

  file(READ "${EpoCanva_Pilot_Manifest}" _pilot_manifest)
  string(JSON _pilot_name  GET "${_pilot_manifest}" output lexicon_file)
  string(JSON _pilot_sha   GET "${_pilot_manifest}" output sha256)
  string(JSON _pilot_bytes GET "${_pilot_manifest}" output size_bytes)
  string(JSON _pilot_rows  GET "${_pilot_manifest}" output line_count)
  string(JSON _pilot_src   GET "${_pilot_manifest}" inputs 0 repo_path)
  string(JSON _pilot_commit GET "${_pilot_manifest}" pinned_commit)
  foreach(_v _pilot_name _pilot_sha _pilot_bytes _pilot_rows _pilot_src _pilot_commit)
    if("${${_v}}" STREQUAL "")
      message(FATAL_ERROR "pilot: ${_v} is empty in ${EpoCanva_Pilot_Manifest}")
    endif()
  endforeach()

  # The name matters as well as the content: pointing the option at some other TSV while the
  # manifest describes another file would otherwise let a wrong list through on a lucky hash.
  get_filename_component(_pilot_given_name "${EPOCANVA_ENGLISH_PILOT_LEXICON}" NAME)
  if(NOT _pilot_given_name STREQUAL _pilot_name)
    message(FATAL_ERROR
      "pilot: EPOCANVA_ENGLISH_PILOT_LEXICON names ${_pilot_given_name}, but the manifest "
      "records the generated list as ${_pilot_name}")
  endif()

  file(SIZE "${EPOCANVA_ENGLISH_PILOT_LEXICON}" _pilot_actual_bytes)
  file(SHA256 "${EPOCANVA_ENGLISH_PILOT_LEXICON}" _pilot_actual_sha)
  if(NOT _pilot_actual_bytes EQUAL _pilot_bytes)
    message(FATAL_ERROR
      "pilot data identity FAILED on size: ${EPOCANVA_ENGLISH_PILOT_LEXICON} is "
      "${_pilot_actual_bytes} bytes, the manifest ${EpoCanva_Pilot_Manifest} expects "
      "${_pilot_bytes}")
  endif()
  if(NOT _pilot_actual_sha STREQUAL _pilot_sha)
    message(FATAL_ERROR
      "pilot data identity FAILED on SHA-256: ${EPOCANVA_ENGLISH_PILOT_LEXICON} hashes to "
      "${_pilot_actual_sha}, the manifest expects ${_pilot_sha}. This configure does not "
      "adjust the expectation to fit the file; regenerate the data with "
      "tools/data/build_english_pilot.py or point the option at the pinned artefact.")
  endif()
  message(STATUS "pilot identity   : OK  sha256 ${_pilot_actual_sha}  ${_pilot_actual_bytes} "
                 "bytes  ${_pilot_rows} rows  source ${_pilot_src} @ ${_pilot_commit}")
endif()

set(_SOURCES
  main.cc
  recovery_test_support.cc
  completion_tests.cc
  case_tests.cc
  token_tests.cc
  space_tests.cc
  lexicon_failure_tests.cc
  spelling_tests.cc
  spelling_commit_tests.cc
  spelling_negative_tests.cc
  pilot_sha256.cc
  pilot_reference.cc
  pilot_tests.cc)
set(_DEFINES RIME_IMPORTS _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
set(_OPTIONS /utf-8 /permissive- /Zc:__cplusplus /W4 /EHsc)

# RIME_IMPORTS is the dllimport view of the API (rime_api.h:23-26); /W4 is kept because
# this project's whole purpose is observing things precisely.
add_executable(rime_english_recovery ${_SOURCES})
target_compile_definitions(rime_english_recovery PRIVATE ${_DEFINES})
target_compile_options(rime_english_recovery PRIVATE ${_OPTIONS})
target_include_directories(rime_english_recovery PRIVATE
  "${Eng_INCLUDE_DIR}" "${EpoCanva_English_Header_Dir}")

# bcrypt is the Windows CNG import library that pilot_sha256.cc resolves its SHA-256 calls
# against, and it is listed on exactly the three targets below because exactly those three
# compile that file. It is not required by rime.lib, it is not referenced anywhere under
# engine/, and the product binary still computes no hash of its own.
target_link_libraries(rime_english_recovery PRIVATE "${Eng_IMPORT_LIB}" bcrypt)
set_target_properties(rime_english_recovery PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_BINARY_DIR}/english-engine")

add_executable(rime_english_recovery_baseline ${_SOURCES})
target_compile_definitions(rime_english_recovery_baseline PRIVATE ${_DEFINES})
target_compile_options(rime_english_recovery_baseline PRIVATE ${_OPTIONS})
target_include_directories(rime_english_recovery_baseline PRIVATE
  "${Base_INCLUDE_DIR}" "${EpoCanva_English_Header_Dir}")
target_link_libraries(rime_english_recovery_baseline PRIVATE "${Base_IMPORT_LIB}" bcrypt)
set_target_properties(rime_english_recovery_baseline PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_BINARY_DIR}/baseline-engine")

# The digest helper's own tests, with no engine in sight: two translation units, no rime.lib
# and no rime.dll in its output directory. Everything it asserts is about bytes on disk, so it
# is registered in BOTH configure modes -- with the pilot switch off it still has to digest the
# vectors correctly, and the only thing the switch adds is the real word list case.
add_executable(rime_english_recovery_sha256
  pilot_sha256.cc
  pilot_sha256_tests.cc)
target_compile_definitions(rime_english_recovery_sha256 PRIVATE
  _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(rime_english_recovery_sha256 PRIVATE ${_OPTIONS})
target_link_libraries(rime_english_recovery_sha256 PRIVATE bcrypt)
set_target_properties(rime_english_recovery_sha256 PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_BINARY_DIR}/sha256-helper")

# The identity expectations the pilot modes check the file on disk against, taken from the
# manifest read above and compiled in. All three executables get them because all three
# compile the helper; only the english-linked one is registered with engine pilot cases, and
# only the helper test adds the real-word-list case. If the quoting were lost the string
# literal would stop being a literal and the build would fail, which is the loud outcome this
# file prefers over a mode that compares against an empty expectation.
if(EPOCANVA_ENGLISH_PILOT_LEXICON)
  set(_pilot_defines
    "EPOCANVA_PILOT_EXPECTED_SHA256=\"${_pilot_sha}\""
    "EPOCANVA_PILOT_EXPECTED_BYTES=${_pilot_bytes}"
    "EPOCANVA_PILOT_EXPECTED_ROWS=${_pilot_rows}"
    "EPOCANVA_PILOT_SOURCE_PATH=\"${_pilot_src}\""
    "EPOCANVA_PILOT_PINNED_COMMIT=\"${_pilot_commit}\"")
  target_compile_definitions(rime_english_recovery PRIVATE ${_pilot_defines})
  target_compile_definitions(rime_english_recovery_baseline PRIVATE ${_pilot_defines})
  target_compile_definitions(rime_english_recovery_sha256 PRIVATE ${_pilot_defines})
endif()

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

# FIXTURE_DIR and EXTRA_ARGS exist for the pilot and spelling cases and default to nothing, so
# every case registered without them keeps the command line it was verified with -- the
# registrations that predate those groups are not re-worded, re-ordered or re-pathed.
function(epocanva_add_case)
  cmake_parse_arguments(CASE "BASELINE_EXE"
                        "NAME;MODE;EXPECTED_EXIT;MARKER;MARKER_POLICY;LABEL;FIXTURE_DIR"
                        "EXTRA_ARGS" ${ARGN})
  if(NOT CASE_MARKER_POLICY)
    set(CASE_MARKER_POLICY require)
  endif()
  if(NOT CASE_FIXTURE_DIR)
    set(CASE_FIXTURE_DIR "${EpoCanva_English_Fixture_Dir}")
  endif()
  if(CASE_BASELINE_EXE)
    set(exe_dir "${CMAKE_CURRENT_BINARY_DIR}/baseline-engine")
    set(exe "$<TARGET_FILE:rime_english_recovery_baseline>")
  else()
    set(exe_dir "${CMAKE_CURRENT_BINARY_DIR}/english-engine")
    set(exe "$<TARGET_FILE:rime_english_recovery>")
  endif()
  set(_extra "")
  foreach(_arg IN LISTS CASE_EXTRA_ARGS)
    string(APPEND _extra ";${_arg}")
  endforeach()
  add_test(
    NAME ${CASE_NAME}
    COMMAND ${CMAKE_COMMAND}
      -DEXE=${exe}
      -DCASE=${CASE_LABEL}
      -DEXPECTED_EXIT=${CASE_EXPECTED_EXIT}
      "-DMARKER=${CASE_MARKER}"
      "-DMARKER_POLICY=${CASE_MARKER_POLICY}"
      "-DPASS_ARGS=--expect-dll;${exe_dir}/rime.dll;--fixture-dir;${CASE_FIXTURE_DIR};--run-root;${_run_root}/${CASE_LABEL};--mode;${CASE_MODE}${_extra}"
      -DTIMEOUT_SECONDS=120
      -P ${_assert}
  )
endfunction()

# The digest helper's program is registered with the SAME wrapper and the SAME judgement --
# exact exit code plus marker policy -- and nothing about the engine, because it starts no
# engine. Its arguments are --run-root and --mode, plus --pilot-lexicon for the one case that
# reads the real word list.
function(epocanva_add_sha256_case)
  cmake_parse_arguments(CASE ""
    "NAME;MODE;EXPECTED_EXIT;MARKER;MARKER_POLICY;LABEL;PILOT_LEXICON" "" ${ARGN})
  if(NOT CASE_MARKER_POLICY)
    set(CASE_MARKER_POLICY require)
  endif()
  set(_extra "")
  if(CASE_PILOT_LEXICON)
    set(_extra ";--pilot-lexicon;${CASE_PILOT_LEXICON}")
  endif()
  add_test(
    NAME ${CASE_NAME}
    COMMAND ${CMAKE_COMMAND}
      -DEXE=$<TARGET_FILE:rime_english_recovery_sha256>
      -DCASE=${CASE_LABEL}
      -DEXPECTED_EXIT=${CASE_EXPECTED_EXIT}
      "-DMARKER=${CASE_MARKER}"
      "-DMARKER_POLICY=${CASE_MARKER_POLICY}"
      "-DPASS_ARGS=--run-root;${_run_root}/${CASE_LABEL};--mode;${CASE_MODE}${_extra}"
      -DTIMEOUT_SECONDS=60
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

# 7. The P2.2 case matrix, same fixture and same engine: three shapes render the same
#    rows, mixed case is refused, an exact entry in another shape dedupes rather than
#    refuses, the default commit keeps the user's own case, a selected completion commits
#    in that shape, BackSpace recomputes the shape, two sessions keep their own, and the
#    whole matrix costs exactly one word-list read.
epocanva_add_case(
  NAME      recovery.case.preservation_and_mapped_commit
  MODE      case-preservation
  EXPECTED_EXIT 0
  MARKER    "RESULT PASS"
  MARKER_POLICY require
  LABEL     case-preservation)

# 8. The case group's own falsification control: the right four words in the wrong order.
#    A menu check that compared only the SET would keep passing here, which is exactly the
#    degradation this case exists to catch.
epocanva_add_case(
  NAME      recovery.case.suite_can_fail_on_menu_ranking
  MODE      case-self-check-fail
  EXPECTED_EXIT 14
  MARKER    "RESULT PASS"
  MARKER_POLICY forbid
  LABEL     case-self-check-fail)

# 9. The P2.3 matrix: a prefix ending in one ASCII delimiter stays completable in all three
#    shapes and commits the whole word on selection; digits and the eleven technical marks
#    come back whole, frame by frame, with no completion and the reason named; a delimiter
#    pattern outside the limited grammar is refused as illegal-word-form, never as
#    no-match and never as technical text; the whole token reaches the translator
#    (queries_covering_part_of_input stays 0); protection is not sticky across BackSpace,
#    Escape or a second session; and 64 code units is still queried while 65 is refused
#    without losing a byte.
epocanva_add_case(
  NAME      recovery.token.boundaries_and_protection
  MODE      token-protection
  EXPECTED_EXIT 0
  MARKER    "RESULT PASS"
  MARKER_POLICY require
  LABEL     token-protection)

# 10. The token group's falsification control: it asserts the exact corruption P2.3 forbids
#     -- the letters of "hel@example.com" completed while the address was being typed -- and
#     must exit 14. This is what proves the group checks candidate CONTENT and token SCOPE
#     rather than only whether the final committed string happens to match.
epocanva_add_case(
  NAME      recovery.token.suite_can_fail_on_protected_text
  MODE      token-self-check-fail
  EXPECTED_EXIT 14
  MARKER    "RESULT PASS"
  MARKER_POLICY forbid
  LABEL     token-self-check-fail)

# 11. The P2.5 matrix, on real process_key() events and real get_commit() reads: the three
#     rules A/B/C, a highlight that must NOT be accepted, a selection that must be committed
#     exactly once, two empty-composition spaces staying two spaces, continuous multi-word
#     input assembled only from returned commits, protected text committing byte-for-byte
#     with completions back on the next word, no case shape inherited across a commit,
#     Escape not revived by a later space, Ctrl/Alt/Super/release spaces refused with the
#     composition intact, BackSpace on an empty composition left to the host, two sessions
#     interleaved, and the four counter identities that make "committed once, then one
#     space" observable at all.
epocanva_add_case(
  NAME      recovery.space.commit_and_multiword
  MODE      space-commit
  EXPECTED_EXIT 0
  MARKER    "RESULT PASS"
  MARKER_POLICY require
  LABEL     space-commit)

# 12. The space group's falsification control: it claims that a space after a highlighted
#     "hello" commits "hello ", which case A forbids. Must exit 14 -- otherwise a space test
#     that only checked "something was committed" would be indistinguishable from one that
#     accepted highlights.
epocanva_add_case(
  NAME      recovery.space.suite_can_fail_on_highlight
  MODE      space-self-check-fail
  EXPECTED_EXIT 14
  MARKER    "RESULT PASS"
  MARKER_POLICY forbid
  LABEL     space-self-check-fail)

# 13-16. The word-list LOAD failures. Each runs in its own process, against its own run
#        root, with only the file at the path the schema names changed -- and each asserts
#        the candidate list AND the load counters AND the status name together, because a
#        failed load still offers the typed text and would otherwise look like a working
#        completer that simply had nothing to say. The english engine is required: an absent
#        module must not be able to impersonate a word-list error, so each mode refuses to
#        run (exit 20) unless the module and its diagnostics are present.
epocanva_add_case(
  NAME      recovery.lexicon.negative_missing_file
  MODE      missing-lexicon
  EXPECTED_EXIT 0
  MARKER    "RESULT PASS"
  MARKER_POLICY require
  LABEL     missing-lexicon)

epocanva_add_case(
  NAME      recovery.lexicon.negative_duplicate_entry
  MODE      bad-lexicon-duplicate
  EXPECTED_EXIT 0
  MARKER    "RESULT PASS"
  MARKER_POLICY require
  LABEL     bad-lexicon-duplicate)

epocanva_add_case(
  NAME      recovery.lexicon.negative_illegal_word
  MODE      bad-lexicon-illegal
  EXPECTED_EXIT 0
  MARKER    "RESULT PASS"
  MARKER_POLICY require
  LABEL     bad-lexicon-illegal)

epocanva_add_case(
  NAME      recovery.lexicon.negative_illegal_word_form
  MODE      bad-lexicon-illegal-form
  EXPECTED_EXIT 0
  MARKER    "RESULT PASS"
  MARKER_POLICY require
  LABEL     bad-lexicon-illegal-form)

# 17. The control that keeps the group honest: the same verification path run against the
#     GOOD 54-row fixture while claiming a duplicate-word load failure. The healthy
#     completion menu is asserted first, so this run can only fail on the load diagnostics
#     -- not on a wrong DLL, a missing module or a bad argument. Exit 14, marker forbidden.
epocanva_add_case(
  NAME      recovery.lexicon.suite_can_fail_on_valid_data
  MODE      lexicon-failure-self-check-fail
  EXPECTED_EXIT 14
  MARKER    "RESULT PASS"
  MARKER_POLICY forbid
  LABEL     lexicon-failure-self-check-fail)

# ------------------------------------------------------------------ spelling cases --
# The P2.6 group, on tests/fixtures/english_spelling/ and its 16 controlled rows. Each gets
# its own run root, and none of them takes the pilot data: the whole group runs with the
# hand-made list, so a green spelling suite never depends on a generated artefact.
#
# 18. The gate table and the candidate list, one keystroke at a time: thirteen named states,
#     suggestions and completions mutually exclusive, the "spell" label as the only thing a
#     front end can see, the 3-suggestion budget inside the same 8-candidate page, the four
#     admitted edits, the weight tie broken by ascending canonical text, three deletions that
#     reach one row shown once, all three accepted case shapes, both length edges and just
#     outside them, technical text and delimited forms refused, and BackSpace recomputing the
#     menu instead of replaying it.
epocanva_add_case(
  NAME      recovery.spelling.suggestions_and_suppressions
  MODE      spelling-suggestions
  FIXTURE_DIR "${EpoCanva_English_Spelling_Fixture_Dir}"
  EXPECTED_EXIT 0
  MARKER    "RESULT PASS"
  MARKER_POLICY require
  LABEL     spelling-suggestions)

# 19. Suggestions at the commit boundary: default Return keeps the raw text, default Space
#     keeps it plus exactly one U+0020, a highlight is not an acceptance, an explicit
#     selection commits the suggested row in the shape that was typed, the commit is consumed
#     once, Escape leaves nothing, two sessions keep their own menus, and the next ordinary
#     word still completes.
epocanva_add_case(
  NAME      recovery.spelling.commit_and_selection
  MODE      spelling-commit
  FIXTURE_DIR "${EpoCanva_English_Spelling_Fixture_Dir}"
  EXPECTED_EXIT 0
  MARKER    "RESULT PASS"
  MARKER_POLICY require
  LABEL     spelling-commit)

# 20. The state that must never be folded into "no suggestions": the schema asked and there is
#     no usable word list. Run against the schema the program derives at staging time, whose
#     three-row list repeats its first row on line 3; the case asserts the candidate list AND
#     the load counters AND the status name together, so a broken list cannot read as a healthy
#     suggester that simply had nothing to say.
epocanva_add_case(
  NAME      recovery.spelling.negative_lexicon_unavailable
  MODE      spelling-lexicon-unavailable
  FIXTURE_DIR "${EpoCanva_English_Spelling_Fixture_Dir}"
  EXPECTED_EXIT 0
  MARKER    "RESULT PASS"
  MARKER_POLICY require
  LABEL     spelling-lexicon-unavailable)

# 21. Control for the ranking: the right three suggestions in the wrong order. A menu check
#     that compared only the set would keep passing, which is the degradation this catches.
epocanva_add_case(
  NAME      recovery.spelling.suite_can_fail_on_suggestion_ranking
  MODE      spelling-ranking-self-check-fail
  FIXTURE_DIR "${EpoCanva_English_Spelling_Fixture_Dir}"
  EXPECTED_EXIT 14
  MARKER    "RESULT PASS"
  MARKER_POLICY forbid
  LABEL     spelling-ranking-self-check-fail)

# 22. Control for the label column: claims the completion "heli -> helium" carries the
#     suggestion label. If labels stopped separating the two kinds, every other case in the
#     group would lose the only distinction the C API exposes -- so this claim must fail.
epocanva_add_case(
  NAME      recovery.spelling.suite_can_fail_on_labels
  MODE      spelling-label-self-check-fail
  FIXTURE_DIR "${EpoCanva_English_Spelling_Fixture_Dir}"
  EXPECTED_EXIT 14
  MARKER    "RESULT PASS"
  MARKER_POLICY forbid
  LABEL     spelling-label-self-check-fail)

# ------------------------------------------------------------------ helper cases --
# The digest helper's own three cases. They start no engine and read no generated data, so
# they are registered in BOTH configure modes -- which is the point: with the pilot switch off
# there is still a green run that proves the CNG backend reproduces digests computed outside
# this repository. Cases 23-25; the switch-on configure adds a twenty-sixth for the real word
# list, inside the pilot block below.
#
# 23. The vectors: the empty file, FIPS 180-4's "abc" and 56-byte sample messages, the 55/56/57
#     padding edges, the 63/64/65 block edges, a full 0x00..0xFF ramp, and the helper's own
#     chunk edges at 65,535/65,536/65,537 bytes -- 15 fixtures, each checked for the bytes on
#     disk, a digest in the manifest's format, and equality with an expectation produced by
#     python hashlib, GNU coreutils sha256sum and PowerShell Get-FileHash. The helper computed
#     none of them.
epocanva_add_sha256_case(
  NAME      recovery.pilot_sha256.known_vectors
  MODE      vectors
  EXPECTED_EXIT 0
  MARKER    "RESULT PASS"
  MARKER_POLICY require
  LABEL     sha256-vectors)

# 24. The failure kinds stay apart. An absent path and a directory are refused as open-failed
#     with NO digest (so neither can equal an expected value); a truncated copy and a same-length
#     one-byte edit of an independent copy are readable files whose digests simply differ, which
#     is the finding the identity check is made of; the four kinds are required to have four
#     different names; and in a pilot-configured build the compiled-in expectation is re-checked
#     to be a well-formed 64-character digest, so an empty one cannot pass for it.
epocanva_add_sha256_case(
  NAME      recovery.pilot_sha256.negative_identity_kinds
  MODE      identity-negatives
  EXPECTED_EXIT 0
  MARKER    "RESULT PASS"
  MARKER_POLICY require
  LABEL     sha256-negatives)

# 25. The control that keeps cases 23 and 24 honest: the same verification path run against a
#     deliberately wrong expectation for "abc" -- the real digest with its last character
#     changed. Must exit 14 and must not print the marker; a vector case that could not fail
#     would be indistinguishable from one that simply never compares anything.
epocanva_add_sha256_case(
  NAME      recovery.pilot_sha256.suite_can_fail_on_wrong_vector
  MODE      self-check-fail
  EXPECTED_EXIT 14
  MARKER    "RESULT PASS"
  MARKER_POLICY forbid
  LABEL     sha256-self-check-fail)

# ------------------------------------------------------------------ pilot cases --
# Registered ONLY when EPOCANVA_ENGLISH_PILOT_LEXICON names a generated file that has
# already passed the configure-time identity check above. With the option empty, none of
# these names exists in CTest and nothing in the project asks for the data -- so the
# default configure keeps exactly the 22 engine cases it had plus the 3 helper cases above,
# and a machine without the pinned word list does not run a quietly-skipped pilot test, it
# simply has no pilot test.
#
# Four engine cases, one data case and one helper case, each with its own run root and its own
# exit code:
# 0 = the trial ran and every reference comparison held, 14 = the group's falsification
# control (a deliberately mis-ranked menu on real data), 22 = pre-validation refused the
# data before the engine opened it. A crash, a missing DLL or a bad argument produces a
# different code and therefore fails the judgement rather than impersonating it.
if(EPOCANVA_ENGLISH_PILOT_LEXICON)
  epocanva_add_case(
    NAME      recovery.pilot.real_lexicon_reference_compared
    MODE      pilot-lexicon
    FIXTURE_DIR "${EpoCanva_English_Pilot_Fixture_Dir}"
    EXTRA_ARGS --pilot-lexicon "${EPOCANVA_ENGLISH_PILOT_LEXICON}"
    EXPECTED_EXIT 0
    MARKER    "RESULT PASS"
    MARKER_POLICY require
    LABEL     pilot-real-lexicon)

  # The control that keeps the pilot honest: the identity check passes, the real list
  # loads, the healthy menu is asserted and passes, and ONE menu is then claimed with two
  # rows of the same weight tier swapped. Exit 14 with the marker forbidden is what shows
  # the comparison has teeth on 42k rows instead of merely running green.
  epocanva_add_case(
    NAME      recovery.pilot.suite_can_fail_on_reference_ranking
    MODE      pilot-assertion-self-check-fail
    FIXTURE_DIR "${EpoCanva_English_Pilot_Fixture_Dir}"
    EXTRA_ARGS --pilot-lexicon "${EPOCANVA_ENGLISH_PILOT_LEXICON}"
    EXPECTED_EXIT 14
    MARKER    "RESULT PASS"
    MARKER_POLICY forbid
    LABEL     pilot-assertion-self-check-fail)

  # Data identity, negative control 1: three quarters of the real list, written as an
  # independent copy inside this run's own directory. It parses cleanly, is strictly
  # ascending and has no duplicate row, so a row count or a grammar check cannot refuse it
  # -- only the digest can. The mode asserts lexicon_load_attempts == 0 on the way out,
  # which is what separates "pre-validation refused it" (22) from "the engine failed to
  # load it" (a load-failure code) from "an expectation disagreed" (14), and it re-verifies
  # the original afterwards to show nothing was repaired in place.
  epocanva_add_case(
    NAME      recovery.pilot.truncated_data_refused_before_the_engine
    MODE      pilot-truncated-rejected
    FIXTURE_DIR "${EpoCanva_English_Pilot_Fixture_Dir}"
    EXTRA_ARGS --pilot-lexicon "${EPOCANVA_ENGLISH_PILOT_LEXICON}"
    EXPECTED_EXIT 22
    MARKER    "RESULT PASS"
    MARKER_POLICY forbid
    LABEL     pilot-truncated-rejected)

  # Data identity, negative control 2: a path that is not there. Same code, different named
  # reason, and again the engine is never asked to read anything -- so this is not a
  # re-labelled file-not-found load test.
  epocanva_add_case(
    NAME      recovery.pilot.absent_data_refused_before_the_engine
    MODE      pilot-missing-rejected
    FIXTURE_DIR "${EpoCanva_English_Pilot_Fixture_Dir}"
    EXTRA_ARGS --pilot-lexicon
      "${CMAKE_CURRENT_BINARY_DIR}/pilot-absence-control/epocanva_en_pilot.tsv"
    EXPECTED_EXIT 22
    MARKER    "RESULT PASS"
    MARKER_POLICY forbid
    LABEL     pilot-missing-rejected)

  # The identity check that is not written by this recovery at all: the surviving P2.4
  # script re-hashes the file on disk against the manifest and re-checks the conversion
  # report's own arithmetic, with CMake's own SHA-256. It is a third implementation next to
  # CNG and next to whatever produced the manifest value, so a wrong backend cannot make this
  # case and the pilot modes agree on something the manifest disagrees with.
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/english_pilot/assert_pilot_output.cmake")
    message(FATAL_ERROR
      "pilot: tests/english_pilot/assert_pilot_output.cmake is missing, so the generated "
      "list cannot be checked against the manifest by the surviving script")
  endif()
  add_test(
    NAME    recovery.pilot.output_matches_the_pinned_manifest
    COMMAND ${CMAKE_COMMAND}
      -DMANIFEST=${EpoCanva_Pilot_Manifest}
      -DOUT_DIR=${EPOCANVA_ENGLISH_PILOT_OUTPUT_DIR}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/english_pilot/assert_pilot_output.cmake
  )

  # The real artefact through the new backend: the helper digests build/english-pilot's word
  # list and compares it with the FULL value read out of the manifest at configure time, plus
  # the manifest's byte count. No expectation here is produced by the thing under test, and
  # nothing under build/english-pilot/ is opened for writing by this case.
  epocanva_add_sha256_case(
    NAME      recovery.pilot_sha256.real_lexicon_digest_matches_the_manifest
    MODE      real-file
    PILOT_LEXICON "${EPOCANVA_ENGLISH_PILOT_LEXICON}"
    EXPECTED_EXIT 0
    MARKER    "RESULT PASS"
    MARKER_POLICY require
    LABEL     sha256-real-lexicon)

  set_tests_properties(
    recovery.pilot.real_lexicon_reference_compared
    recovery.pilot.suite_can_fail_on_reference_ranking
    recovery.pilot.truncated_data_refused_before_the_engine
    recovery.pilot.absent_data_refused_before_the_engine
    PROPERTIES TIMEOUT 300
               LABELS "librime;english;completion;case;token;space;lexicon;recovery;pilot")
  set_tests_properties(
    recovery.pilot.output_matches_the_pinned_manifest
    PROPERTIES TIMEOUT 120 LABELS "english;recovery;pilot;data")
  set_tests_properties(
    recovery.pilot_sha256.real_lexicon_digest_matches_the_manifest
    PROPERTIES TIMEOUT 120 LABELS "english;recovery;pilot;digest")
endif()

set_tests_properties(
  recovery.pilot_sha256.known_vectors
  recovery.pilot_sha256.negative_identity_kinds
  recovery.pilot_sha256.suite_can_fail_on_wrong_vector
  PROPERTIES TIMEOUT 120
             LABELS "english;recovery;digest")

set_tests_properties(
  recovery.completion.prefix_candidates_and_commit
  recovery.completion.negative_baseline_engine
  recovery.completion.positive_mode_fails_on_baseline_engine
  recovery.completion.negative_mode_fails_on_english_engine
  recovery.completion.rejects_unknown_mode
  recovery.completion.suite_can_fail
  recovery.case.preservation_and_mapped_commit
  recovery.case.suite_can_fail_on_menu_ranking
  recovery.token.boundaries_and_protection
  recovery.token.suite_can_fail_on_protected_text
  recovery.space.commit_and_multiword
  recovery.space.suite_can_fail_on_highlight
  recovery.lexicon.negative_missing_file
  recovery.lexicon.negative_duplicate_entry
  recovery.lexicon.negative_illegal_word
  recovery.lexicon.negative_illegal_word_form
  recovery.lexicon.suite_can_fail_on_valid_data
  recovery.spelling.suggestions_and_suppressions
  recovery.spelling.commit_and_selection
  recovery.spelling.negative_lexicon_unavailable
  recovery.spelling.suite_can_fail_on_suggestion_ranking
  recovery.spelling.suite_can_fail_on_labels
  PROPERTIES TIMEOUT 180
             LABELS "librime;english;completion;case;token;space;lexicon;spelling;recovery")

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
message(STATUS "  spelling fixture dir : ${EpoCanva_English_Spelling_Fixture_Dir}")
message(STATUS "  assert wrapper       : ${EpoCanva_Assert_Script}")
message(STATUS "  run root             : ${_run_root}")
message(STATUS "  digest backend       : Windows CNG (bcrypt) through pilot_sha256.cc; bcrypt "
               "is linked to the 3 test executables that compile it, and to no library")
message(STATUS "  helper cases         : 3 registered in BOTH modes (vectors, negative "
               "identity kinds, falsification control)")
if(EPOCANVA_ENGLISH_PILOT_LEXICON)
  list(LENGTH _pilot_defines _pilot_define_count)
  message(STATUS "  pilot lexicon        : ${EPOCANVA_ENGLISH_PILOT_LEXICON}")
  message(STATUS "  pilot output dir     : ${EPOCANVA_ENGLISH_PILOT_OUTPUT_DIR}")
  message(STATUS "  pilot fixture dir    : ${EpoCanva_English_Pilot_Fixture_Dir}")
  message(STATUS "  pilot manifest       : ${EpoCanva_Pilot_Manifest}")
  message(STATUS "  pilot defines        : ${_pilot_define_count} identity expectations "
                 "compiled in")
  message(STATUS "  pilot cases          : 6 registered (4 engine modes + 1 data check + "
                 "1 real-list digest)")
  message(STATUS "  ctest total          : 31 = 17 pre-spelling engine cases + 5 spelling "
                 "engine cases + 4 pilot engine cases + 1 pilot data check + 4 helper cases")
else()
  message(STATUS "  pilot lexicon        : not configured -- no pilot case is registered, "
                 "no generated data is required, no manifest is read")
  message(STATUS "  ctest total          : 25 = 22 engine cases (17 pre-spelling + 5 "
                 "spelling) + 3 helper cases")
endif()
