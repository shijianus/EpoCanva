# EpoCanva P2.1 -- CTest cases for the English language core.
#
# These run the core directly (no engine, no librime). The Rime-level behaviour is
# proved separately by tools/rime_english_recovery (run through
# tools/test_english_integration.ps1), which needs a real engine.
#
# The assertion wrapper is the one shipped in P0.1 (tests/infrastructure/
# assert_probe.cmake): exact exit code plus success-marker policy, no WILL_FAIL.
# WILL_FAIL would accept any non-zero exit, including "the test binary did not
# start", so the force-fail case names the exit code it expects instead.

# --- the test binary ------------------------------------------------------------
add_executable(epocanva_english_core_tests main.cc)
target_link_libraries(epocanva_english_core_tests PRIVATE epocanva_en_core)
set_target_properties(epocanva_english_core_tests PROPERTIES FOLDER "epocanva")

# The engine core sources are compiled once into the library above; this asserts
# the test binary really is linked against them rather than a copy that can drift.
if(NOT TARGET epocanva_english_core_tests)
  message(FATAL_ERROR
    "epocanva_english_core_tests is missing. Add this directory from the "
    "top-level EpoCanva project with EPOCANVA_EN_BUILD_STANDALONE=ON.")
endif()

set(EPOCANVA_ASSERT_PROBE_SCRIPT
    "${CMAKE_SOURCE_DIR}/tests/infrastructure/assert_probe.cmake")
if(NOT EXISTS "${EPOCANVA_ASSERT_PROBE_SCRIPT}")
  message(FATAL_ERROR "missing test wrapper: ${EPOCANVA_ASSERT_PROBE_SCRIPT}")
endif()

# 1. The whole unit suite: exit 0 AND the success marker.
add_test(
  NAME english.core.lexicon_and_completion_unit_tests
  COMMAND ${CMAKE_COMMAND}
    -DPROBE=$<TARGET_FILE:epocanva_english_core_tests>
    -DCASE=unit
    -DPROBE_ARG1=--unit
    -DEXPECTED_EXIT=0
    -DEXPECT_MARKER=TRUE
    "-DMARKER=RESULT PASS"
    -DTIMEOUT_SECONDS=60
    -P ${EPOCANVA_ASSERT_PROBE_SCRIPT}
)

# 2. The suite can fail: one intentionally false assertion must be recorded and
#    must produce exit 1. Without this case, a green run of case 1 would not prove
#    the assertions are wired to the exit code at all.
add_test(
  NAME english.core.unit_suite_can_fail
  COMMAND ${CMAKE_COMMAND}
    -DPROBE=$<TARGET_FILE:epocanva_english_core_tests>
    -DCASE=forcefail
    -DPROBE_ARG1=--forcefail
    -DEXPECTED_EXIT=1
    -DEXPECT_MARKER=FALSE
    "-DMARKER=RESULT PASS"
    -DTIMEOUT_SECONDS=60
    -P ${EPOCANVA_ASSERT_PROBE_SCRIPT}
)

# 3. Bad arguments are refused, not ignored: a lost flag would otherwise make the
#    two cases above indistinguishable.
add_test(
  NAME english.core.rejects_unknown_arguments
  COMMAND ${CMAKE_COMMAND}
    -DPROBE=$<TARGET_FILE:epocanva_english_core_tests>
    -DCASE=bad_args
    -DPROBE_ARG1=--not-a-real-mode
    -DEXPECTED_EXIT=2
    -DEXPECT_MARKER=FALSE
    "-DMARKER=RESULT PASS"
    -DTIMEOUT_SECONDS=60
    -P ${EPOCANVA_ASSERT_PROBE_SCRIPT}
)

# The file-loading cases write a small scratch directory. Its default is the OS
# temporary directory (tests/english_core/main.cc), never a name resolved against
# the current directory, and CTest additionally pins the working directory to the
# build tree so nothing this suite produces lands in the source tree.
set_tests_properties(
  english.core.lexicon_and_completion_unit_tests
  english.core.unit_suite_can_fail
  english.core.rejects_unknown_arguments
  PROPERTIES
    TIMEOUT 120
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/run"
    LABELS "english;core;unit"
)

# WORKING_DIRECTORY must exist before execute_process runs in it.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/run")

# 4. P2.6 section 9, the core half of the spelling cost trial: --pilot-bench loads the
#    generated 42,356-row pilot list and measures the one-edit pass per input class,
#    asserting the outcome, the variant count (from the closed arithmetic form, not from
#    the generator) and a checked worst-case ceiling. Registered only when that list
#    exists, on the same policy as the pilot engine cases: a machine that has not
#    generated it registers nothing rather than a test that silently skips, and
#    docs/reports/integration-harness-recovery-status.md records which cases ran and on
#    which configuration. Note the scope: this is the CORE half of the P2.6 cost trial.
#    The engine half was ever only registered, never implemented, and is not registered
#    here -- see the matrix's class-C row for pilot-spelling.
set(EPOCANVA_ENGLISH_PILOT_LEXICON "" CACHE FILEPATH
    "Absolute path of the generated pilot word list for the P2.6 cost trial")
if(EPOCANVA_ENGLISH_PILOT_LEXICON)
  set(_pilot_bench_lexicon "${EPOCANVA_ENGLISH_PILOT_LEXICON}")
else()
  set(_pilot_bench_lexicon
      "${CMAKE_SOURCE_DIR}/build/english-pilot/epocanva_en_pilot.tsv")
endif()
if(EXISTS "${_pilot_bench_lexicon}")
  add_test(
    NAME english.core.spelling_cost_trial_on_pilot_lexicon
    COMMAND ${CMAKE_COMMAND}
      -DPROBE=$<TARGET_FILE:epocanva_english_core_tests>
      -DCASE=pilot-bench
      -DPROBE_ARG1=--pilot-bench
      "-DPROBE_ARG2=${_pilot_bench_lexicon}"
      -DEXPECTED_EXIT=0
      -DEXPECT_MARKER=TRUE
      "-DMARKER=RESULT PASS"
      -DTIMEOUT_SECONDS=240
      -P ${EPOCANVA_ASSERT_PROBE_SCRIPT}
  )
  set_tests_properties(
    english.core.spelling_cost_trial_on_pilot_lexicon
    PROPERTIES
      TIMEOUT 300
      WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/run"
      LABELS "english;core;pilot;performance"
  )
  message(STATUS "english.core: pilot cost trial registered against ${_pilot_bench_lexicon}")
else()
  message(STATUS
    "english.core: no pilot word list at ${_pilot_bench_lexicon}; "
    "the P2.6 cost trial is not registered")
endif()
