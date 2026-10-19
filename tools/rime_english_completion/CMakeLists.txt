# EpoCanva P2.1 -- standalone project for the English prefix-completion test.
#
# Deliberately NOT add_subdirectory()'d into the root EpoCanva project, for the
# reason recorded in P1.3: the root project is a Debug / dynamic-CRT build, while
# this program must link the Release / static-CRT rime.lib. Keeping them apart is
# what stops the two CRTs leaking into each other.
#
# Two executables are built on purpose from one source file:
#
#   rime_english_completion          links the ENGLISH build's rime.lib and runs
#                                    with the english-built rime.dll next to it
#   rime_english_completion_baseline links the unmodified BASELINE rime.lib and runs
#                                    with the baseline rime.dll next to it
#
# Each lives in its own output directory because Windows resolves an implicitly
# loaded DLL from the executable's own directory first, and only one file can be
# named rime.dll there. Two processes, two engines, no PATH involvement -- and each
# program still verifies at run time which module supplied rime_get_api().
#
# Configure with:
#   cmake -S tools/rime_english_completion \
#         -B build/librime-english/test \
#         -G "Visual Studio 17 2022" -A x64 \
#         -DCMAKE_CONFIGURATION_TYPES:STRING=Release \
#         -DRime_Install_Prefix=<repo>/build/librime-english/rime/install \
#         -DBaseline_Rime_Prefix=<repo>/build/librime-baseline/rime/install

cmake_minimum_required(VERSION 3.25)
project(epocanva_rime_english_completion VERSION 0.1.0 LANGUAGES CXX)

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
    "The P1.2/P1.4/P2.1 librime artefacts are Windows/MSVC builds; these programs "
    "link their import libraries.")
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
  "Installation prefix of the ENGLISH build (build/librime-english/rime/install)")
set(Baseline_Rime_Prefix "" CACHE PATH
  "Installation prefix of the unmodified baseline build")
set(EpoCanva_English_Header_Dir
    "${CMAKE_CURRENT_SOURCE_DIR}/../../engine/rime_components/english" CACHE PATH
  "Directory holding english_diagnostics.h, the C surface of the component")
set(EpoCanva_English_Fixture_Dir
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fixtures/english_completion" CACHE PATH
  "Directory holding default.yaml, the schema and the word lists")
# P2.6: the spelling-suggestion fixture set is a separate directory with its own
# schema id, its own word list and its own run root, because the schema that turns
# suggestions on must not be the schema the eleven older cases run against.
set(EpoCanva_English_Spelling_Fixture_Dir
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fixtures/english_spelling" CACHE PATH
  "Directory holding the P2.6 spelling schema, its default.yaml and its 16-row list")
set(EpoCanva_Assert_Script
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/english_completion/assert_exit.cmake"
    CACHE FILEPATH "Wrapper that compares an exact exit code and a marker policy")

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
  # elsewhere on the machine; the loop additionally proves each hit is inside the
  # prefix it was asked for.
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

epocanva_locate_engine(ENGLISH   Rime_Install_Prefix     Eng)
epocanva_locate_engine(BASELINE  Baseline_Rime_Prefix    Base)

# --- versioned inputs must exist, or the build fails at configure time ----------
foreach(_f
        "${EpoCanva_English_Header_Dir}/english_diagnostics.h"
        "${EpoCanva_English_Fixture_Dir}/default.yaml"
        "${EpoCanva_English_Fixture_Dir}/epocanva_test_english.schema.yaml"
        "${EpoCanva_English_Fixture_Dir}/lexicon.tsv"
        "${EpoCanva_English_Fixture_Dir}/lexicon_bad_duplicate.tsv"
        "${EpoCanva_English_Fixture_Dir}/lexicon_bad_illegal.tsv"
        "${EpoCanva_English_Fixture_Dir}/README.md"
        "${EpoCanva_English_Spelling_Fixture_Dir}/default.yaml"
        "${EpoCanva_English_Spelling_Fixture_Dir}/epocanva_spell_english.schema.yaml"
        "${EpoCanva_English_Spelling_Fixture_Dir}/lexicon_spell.tsv"
        "${EpoCanva_English_Spelling_Fixture_Dir}/README.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/main.cc"
        "${EpoCanva_Assert_Script}")
  if(NOT EXISTS "${_f}")
    message(FATAL_ERROR "required input missing: ${_f}")
  endif()
endforeach()

add_executable(rime_english_completion main.cc)
target_compile_definitions(rime_english_completion PRIVATE
  RIME_IMPORTS            # rime_api.h:23-26 -- dllimport view of the API
  _CRT_SECURE_NO_WARNINGS
  NOMINMAX
  WIN32_LEAN_AND_MEAN)
target_compile_options(rime_english_completion PRIVATE
  /utf-8 /permissive- /Zc:__cplusplus /W4 /EHsc)
target_include_directories(rime_english_completion PRIVATE
  "${Eng_INCLUDE_DIR}" "${EpoCanva_English_Header_Dir}")
target_link_libraries(rime_english_completion PRIVATE "${Eng_IMPORT_LIB}")
set_target_properties(rime_english_completion PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_BINARY_DIR}/english-engine")

add_executable(rime_english_completion_baseline main.cc)
target_compile_definitions(rime_english_completion_baseline PRIVATE
  RIME_IMPORTS _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(rime_english_completion_baseline PRIVATE
  /utf-8 /permissive- /Zc:__cplusplus /W4 /EHsc)
target_include_directories(rime_english_completion_baseline PRIVATE
  "${Base_INCLUDE_DIR}" "${EpoCanva_English_Header_Dir}")
target_link_libraries(rime_english_completion_baseline PRIVATE "${Base_IMPORT_LIB}")
set_target_properties(rime_english_completion_baseline PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_BINARY_DIR}/baseline-engine")

# Place the exact built rime.dll in each executable's own directory, so the load
# path is explicit and independent of PATH. Each program then verifies at run time
# that the module which supplied rime_get_api() really is that file.
add_custom_command(TARGET rime_english_completion POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${Eng_SHARED_LIB}"
    "$<TARGET_FILE_DIR:rime_english_completion>/rime.dll"
  COMMENT "Placing the english-built rime.dll next to the completion test")
add_custom_command(TARGET rime_english_completion_baseline POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${Base_SHARED_LIB}"
    "$<TARGET_FILE_DIR:rime_english_completion_baseline>/rime.dll"
  COMMENT "Placing the unmodified baseline rime.dll next to the negative test")

enable_testing()

# Every run root sits under one directory named rime-english-completion-run; the
# program deletes a path only when its parent has exactly that name.
set(_run_root "${CMAKE_CURRENT_BINARY_DIR}/rime-english-completion-run")
set(_assert ${EpoCanva_Assert_Script})

# FIXTURE_DIR and EXTRA_ARGS exist for the P2.4 pilot cases and default to nothing,
# so the 11 cases registered with this function keep exactly the command line they
# were verified with.
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
    set(exe "$<TARGET_FILE:rime_english_completion_baseline>")
  else()
    set(exe_dir "${CMAKE_CURRENT_BINARY_DIR}/english-engine")
    set(exe "$<TARGET_FILE:rime_english_completion>")
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

# 1. The positive run. Exit 0 and the PASS marker.
epocanva_add_case(
  NAME      english.completion.prefix_candidates_and_commit
  MODE      completion
  EXPECTED_EXIT 0
  MARKER    "RESULT PASS"
  MARKER_POLICY require
  LABEL     completion)

# 2. The case matrix: lowercase / initial-capital / uppercase / a single letter /
#    mixed case, with the original text preserved and a mapped completion committed.
epocanva_add_case(
  NAME      english.completion.case_preservation_and_mapped_commit
  MODE      case-preservation
  EXPECTED_EXIT 0
  MARKER    "RESULT PASS"
  MARKER_POLICY require
  LABEL     case-preservation)

# 2b. The P2.3 matrix: delimited prefixes (don', co-, can', re-) in three case
#     shapes, selection committing the mapped word, technical text (e-mail, URL,
#     Windows path, snake_case, version numbers) offered back whole with NO partial
#     completion, illegal word forms refused as their own class, recovery after
#     backspace, an over-long input that loses no bytes, and session isolation --
#     including the counter that proves every query was about the WHOLE composition.
epocanva_add_case(
  NAME      english.completion.token_boundaries_and_protection
  MODE      token-protection
  EXPECTED_EXIT 0
  MARKER    "RESULT PASS"
  MARKER_POLICY require
  LABEL     token-protection)

# 2c. The P2.5 matrix: what the space bar commits. Every step reads the raw
#     composition (get_input, NOT preedit), the candidate menu, the commit queue
#     entries and the processor's own case counters, so "one word then one space,
#     never the word twice" is checked as four separate facts. Includes the two
#     sessions, the modifier combinations that must not be accepted, and a highlight
#     that must NOT be treated as acceptance.
epocanva_add_case(
  NAME      english.completion.space_boundaries_and_multi_word_commit
  MODE      space-commit
  EXPECTED_EXIT 0
  MARKER    "RESULT PASS"
  MARKER_POLICY require
  LABEL     space-commit)

# 3-5. Word-list failures, each named by status and line.
epocanva_add_case(
  NAME      english.completion.negative_missing_lexicon
  MODE      missing-lexicon
  EXPECTED_EXIT 0
  MARKER    "RESULT PASS"
  MARKER_POLICY require
  LABEL     missing-lexicon)
epocanva_add_case(
  NAME      english.completion.negative_bad_lexicon_duplicate
  MODE      bad-lexicon-duplicate
  EXPECTED_EXIT 0
  MARKER    "RESULT PASS"
  MARKER_POLICY require
  LABEL     bad-lexicon-duplicate)
epocanva_add_case(
  NAME      english.completion.negative_bad_lexicon_illegal
  MODE      bad-lexicon-illegal
  EXPECTED_EXIT 0
  MARKER    "RESULT PASS"
  MARKER_POLICY require
  LABEL     bad-lexicon-illegal)

# 5b. The P2.3 word-form failure, through the real engine: a row whose bytes are
#     all in the word alphabet but whose delimiter placement is not a word. It must
#     arrive as its OWN status ("illegal-word-form") on its own line, never as
#     "illegal-word" and never as an empty-but-successful load.
epocanva_add_case(
  NAME      english.completion.negative_bad_lexicon_illegal_form
  MODE      bad-lexicon-illegal-form
  EXPECTED_EXIT 0
  MARKER    "RESULT PASS"
  MARKER_POLICY require
  LABEL     bad-lexicon-illegal-form)

# 6. The unmodified baseline engine must show no module and no candidates.
epocanva_add_case(
  NAME      english.completion.negative_baseline_engine
  BASELINE_EXE  TRUE
  MODE      expect-module-absent
  EXPECTED_EXIT 0
  MARKER    "RESULT PASS"
  MARKER_POLICY require
  LABEL     expect-module-absent)

# 7. THE POSITIVE TEST MUST FAIL ON THE WRONG ENGINE. Same program, same mode,
#    baseline rime.dll: the module is absent, so it must exit 20. Without this
#    case, a green run of case 1 would not prove the assertions are wired to the
#    engine binary rather than to the --mode string.
epocanva_add_case(
  NAME      english.completion.positive_mode_fails_on_baseline_engine
  BASELINE_EXE  TRUE
  MODE      completion
  EXPECTED_EXIT 20
  MARKER    "RESULT PASS"
  MARKER_POLICY forbid
  LABEL     completion-on-baseline)

# 8. THE NEGATIVE TEST MUST FAIL ON THE WRONG ENGINE. The english-linked program
#    asked to assert an absence finds the module present, so it must exit 18.
epocanva_add_case(
  NAME      english.completion.negative_mode_fails_on_english_engine
  MODE      expect-module-absent
  EXPECTED_EXIT 18
  MARKER    "RESULT PASS"
  MARKER_POLICY forbid
  LABEL     expect-module-absent-on-english)

# 8b. THE P2.6 SPELLING MATRIX, run against tests/fixtures/english_spelling: the four
#     one-edit operations isolated, the nine suppression states named per keystroke, the
#     fixed label column that separates a suggestion from a completion through the C API,
#     the budget and length window edges, the three ASCII case shapes, the commit
#     behaviour with and without an explicit selection, backspace recomputation, session
#     isolation, and a word list that cannot be loaded reported as a load failure rather
#     than as "nothing to suggest". Its schema is the only one that sets
#     english/enable_suggestions.
epocanva_add_case(
  NAME      english.completion.spelling_suggestions_and_suppressions
  MODE      spelling
  EXPECTED_EXIT 0
  MARKER    "RESULT PASS"
  MARKER_POLICY require
  LABEL     spelling
  FIXTURE_DIR "${EpoCanva_English_Spelling_Fixture_Dir}")

# 9. Bad arguments are refused rather than ignored.
epocanva_add_case(
  NAME      english.completion.rejects_unknown_mode
  MODE      not-a-real-mode
  EXPECTED_EXIT 2
  MARKER    "RESULT PASS"
  MARKER_POLICY forbid
  LABEL     unknown-mode)

# Timeout and labels for every case this project registers. P2.3's two cases and
# P2.5's space case are listed here as well: they were registered without being
# added, which left them with the wrapper's default timeout and no LABELS, so
# `ctest -L completion` silently omitted them.
set_tests_properties(
  english.completion.prefix_candidates_and_commit
  english.completion.case_preservation_and_mapped_commit
  english.completion.token_boundaries_and_protection
  english.completion.space_boundaries_and_multi_word_commit
  english.completion.spelling_suggestions_and_suppressions
  english.completion.negative_missing_lexicon
  english.completion.negative_bad_lexicon_duplicate
  english.completion.negative_bad_lexicon_illegal
  english.completion.negative_bad_lexicon_illegal_form
  english.completion.negative_baseline_engine
  english.completion.positive_mode_fails_on_baseline_engine
  english.completion.negative_mode_fails_on_english_engine
  english.completion.rejects_unknown_mode
  PROPERTIES TIMEOUT 180 LABELS "librime;english;completion;spelling")

# The wrapper's own timeout is one notch below the CTest TIMEOUT so a hung program
# is reported by the wrapper (non-numeric result) before CTest kills the run.

# ------------------------------------------------------------------ P2.4 pilot --
# The pilot cases are registered ONLY when the configure step names a generated
# pilot word list. That is deliberate: a machine that has not fetched the pinned
# English Speller Database commit must not see a quietly-skipped pilot test, and it
# must not see a failing one either. The default 11 cases above are unaffected, and
# the pilot gets its own binary directory, its own fixture directory and its own run
# root:
#   python tools/data/build_english_pilot.py --fetch
#   cmake -S tools/rime_english_completion -B build/english-pilot/test \
#         -DEPOCANVA_ENGLISH_PILOT_LEXICON=<abs>/build/english-pilot/epocanva_en_pilot.tsv
#   cmake --build build/english-pilot/test --config Release
#   ctest --test-dir build/english-pilot/test -C Release
set(EPOCANVA_ENGLISH_PILOT_LEXICON "" CACHE FILEPATH
    "Absolute path of a generated pilot word list; empty registers no pilot case")
set(EPOCANVA_ENGLISH_PILOT_OUTPUT_DIR "" CACHE PATH
    "Directory holding the generated pilot lexicon and its conversion report")
if(EPOCANVA_ENGLISH_PILOT_LEXICON AND NOT EPOCANVA_ENGLISH_PILOT_OUTPUT_DIR)
  # Default to the directory the word list was generated into.
  get_filename_component(EPOCANVA_ENGLISH_PILOT_OUTPUT_DIR
    "${EPOCANVA_ENGLISH_PILOT_LEXICON}" DIRECTORY)
endif()
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
      "tools/data/build_english_pilot.py first.")
  endif()
  # The pilot run roots live under this binary directory, i.e. under
  # build/english-pilot/test/, not next to the hand-written fixture's runs.
  epocanva_add_case(
    NAME      english.pilot.real_lexicon_candidates_commit_and_protection
    MODE      pilot-lexicon
    FIXTURE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fixtures/english_pilot"
    EXTRA_ARGS --pilot-lexicon "${EPOCANVA_ENGLISH_PILOT_LEXICON}"
    EXPECTED_EXIT 0
    MARKER    "RESULT PASS"
    MARKER_POLICY require
    LABEL     pilot-real-lexicon)

  # P2.6 section 9, the engine half of the cost trial: the same 13 inputs the core bench
  # uses, typed twice through process_key() -- once in the pilot schema and once in a
  # generated twin whose only difference is english/enable_suggestions -- with a
  # per-key timing distribution reported for each. Registered under the same condition as
  # the other pilot cases, because it cannot run without the generated word list.
  epocanva_add_case(
    NAME      english.pilot.spelling_suggestion_cost_a_b_on_pilot_data
    MODE      pilot-spelling
    FIXTURE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fixtures/english_pilot"
    EXTRA_ARGS --pilot-lexicon "${EPOCANVA_ENGLISH_PILOT_LEXICON}"
    EXPECTED_EXIT 0
    MARKER    "RESULT PASS"
    MARKER_POLICY require
    LABEL     pilot-spelling-cost)

  # Second pilot case, and it is deliberately NOT an engine case: the generated list
  # must be the list the manifest says it is. tests/english_pilot/
  # assert_pilot_output.cmake re-hashes the file on disk against
  # data/manifests/english-pilot.sources.lock.json and re-checks the conversion
  # report's own arithmetic (accepted + every exclusion == source rows). It can fail
  # on a stale or truncated file, on an edited manifest, and on a converter that
  # stopped counting.
  add_test(
    NAME    english.pilot.output_matches_the_pinned_manifest
    COMMAND ${CMAKE_COMMAND}
      -DMANIFEST=${CMAKE_CURRENT_SOURCE_DIR}/../../data/manifests/english-pilot.sources.lock.json
      -DOUT_DIR=${EPOCANVA_ENGLISH_PILOT_OUTPUT_DIR}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/english_pilot/assert_pilot_output.cmake
  )

  set_tests_properties(
    english.pilot.real_lexicon_candidates_commit_and_protection
    english.pilot.spelling_suggestion_cost_a_b_on_pilot_data
    PROPERTIES TIMEOUT 300 LABELS "librime;english;pilot")
  set_tests_properties(
    english.pilot.output_matches_the_pinned_manifest
    PROPERTIES TIMEOUT 120 LABELS "english;pilot;data")
endif()

message(STATUS "epocanva_rime_english_completion summary")
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
