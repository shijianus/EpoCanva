# EpoCanva P3.3 stage 1 -- the pure decision verification program and its CTest cases.
#
# NOT A STANDALONE PROJECT. This directory is included by platforms/windows/input/
# CMakeLists.txt. The guard below keeps it from being configured alone, where it would have no
# library to judge and no CRT agreement with it.
#
# The program links the pure static library directly: there is no DLL, no COM and no system
# call in this stage, so linking is the honest coupling. The Windows API adapter has a
# separate program and separate cases; this file deliberately does not share a target with it,
# so a green pure run cannot be reported as a real-layout result.
#
# Judging uses the repository's existing wrapper tests/english_completion/assert_exit.cmake
# (exact exit code + marker policy), the same generic harness P3.1 and P3.2 reused.

if(NOT TARGET epocanva_key_decisions)
  message(FATAL_ERROR
    "tests/windows/input_decision must be included from platforms/windows/input (the "
    "top-level project that builds epocanva_key_decisions); it is not configurable alone.")
endif()

add_executable(input_decision_tests main.cc)
target_compile_definitions(input_decision_tests PRIVATE
  _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(input_decision_tests PRIVATE
  /W4 /permissive- /EHsc /Zc:__cplusplus /utf-8)
target_link_libraries(input_decision_tests PRIVATE epocanva_key_decisions)

enable_testing()

set(_input_assert "${CMAKE_CURRENT_LIST_DIR}/../../english_completion/assert_exit.cmake")

# 1. The full pure table: the 76-character schema alphabet, the four special keys, shortcut
#    and privacy precedence, dead/no/multiple/non-ASCII refusals, boundary demands, release
#    and repeat metadata, and three identical evaluations of one input.
add_test(
  NAME windows.input.decisions
  COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:input_decision_tests>
    -DCASE=decisions
    -DEXPECTED_EXIT=0
    "-DMARKER=RESULT PASS"
    -DMARKER_POLICY=require
    "-DPASS_ARGS=--mode;decisions"
    -DTIMEOUT_SECONDS=120
    -P ${_input_assert}
)

# 2. The falsifiability control: one deliberately false claim (that a dead key becomes a text
#    candidate) must exit 14 with no RESULT PASS. Without it, a green decisions run whose
#    checks had all become tautologies would be indistinguishable from this one.
add_test(
  NAME windows.input.decisions_can_fail
  COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:input_decision_tests>
    -DCASE=decisions-can-fail
    -DEXPECTED_EXIT=14
    "-DMARKER=RESULT PASS"
    -DMARKER_POLICY=forbid
    "-DPASS_ARGS=--mode;self-check-fail"
    -DTIMEOUT_SECONDS=120
    -P ${_input_assert}
)

set_tests_properties(windows.input.decisions windows.input.decisions_can_fail
  PROPERTIES TIMEOUT 180
             LABELS "windows;input;keyboard;decision;pure")

message(STATUS "  input_decision cases : 2 (decisions, decisions_can_fail)")
