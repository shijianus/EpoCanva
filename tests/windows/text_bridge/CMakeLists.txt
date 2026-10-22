# EpoCanva P3.2 -- the text adapter verification program and its CTest cases.
#
# NOT A STANDALONE PROJECT. This directory is included by platforms/windows/text/
# CMakeLists.txt (one configure builds the libraries and this program, one build directory,
# one CTest). The guard below keeps it from being configured alone, where it would have no
# libraries to judge and no CRT agreement with them.
#
# The program links the two static libraries directly -- unlike tests/windows/tsf_lifecycle,
# which reaches a DLL through LoadLibrary because that is the only way an unregistered COM
# server can be exercised. Here there is no DLL and no COM: the unit under test is a pure
# function pair, so linking it is the honest coupling and there is no search-order risk.
#
# The three CTest cases judge through the repository's existing wrapper
# tests/english_completion/assert_exit.cmake (exact exit code + marker policy), the same
# generic harness P3.1 reused. No new test framework, and no Windows rule added to any
# English source file.

if(NOT TARGET epocanva_text_encoding)
  message(FATAL_ERROR
    "tests/windows/text_bridge must be included from platforms/windows/text (the top-level "
    "project that builds epocanva_text_encoding); it is not configurable alone.")
endif()

add_executable(text_bridge main.cc)
target_compile_definitions(text_bridge PRIVATE
  _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_compile_options(text_bridge PRIVATE
  /W4 /permissive- /EHsc /Zc:__cplusplus /utf-8)
target_link_libraries(text_bridge PRIVATE epocanva_text_encoding)

enable_testing()

set(_text_assert "${CMAKE_CURRENT_LIST_DIR}/../../english_completion/assert_exit.cmake")

# 1. The strict conversions: every legal case checked against hand-written byte and code
#    unit expectations (not against the other direction of the same implementation), every
#    illegal UTF-8 and UTF-16 case refused, embedded NULs preserved, a failure never
#    reported as an empty success, and no diagnostic carrying the input text.
add_test(
  NAME windows.text.encoding_strict
  COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:text_bridge>
    -DCASE=encoding-strict
    -DEXPECTED_EXIT=0
    "-DMARKER=RESULT PASS"
    -DMARKER_POLICY=require
    "-DPASS_ARGS=--mode;encoding"
    -DTIMEOUT_SECONDS=120
    -P ${_text_assert}
)

# 2. The length guards: pure arithmetic on the int boundary the Win32 APIs impose, driven
#    with INT_MAX, INT_MAX+1 and SIZE_MAX. No allocation and no invalid pointer is ever
#    dereferenced -- the guard runs before the input is touched, and this case is what
#    proves it does.
add_test(
  NAME windows.text.length_guards
  COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:text_bridge>
    -DCASE=length-guards
    -DEXPECTED_EXIT=0
    "-DMARKER=RESULT PASS"
    -DMARKER_POLICY=require
    "-DPASS_ARGS=--mode;guards"
    -DTIMEOUT_SECONDS=120
    -P ${_text_assert}
)

# 3. The falsifiability control: one deliberately false claim (that a truncated UTF-8
#    sequence converts successfully) must exit 14 with no RESULT PASS. Without it, a green
#    encoding run whose checks were all tautologies would be indistinguishable from this one.
add_test(
  NAME windows.text.suite_can_fail
  COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:text_bridge>
    -DCASE=self-check-fail
    -DEXPECTED_EXIT=14
    "-DMARKER=RESULT PASS"
    -DMARKER_POLICY=forbid
    "-DPASS_ARGS=--mode;self-check-fail"
    -DTIMEOUT_SECONDS=120
    -P ${_text_assert}
)

set_tests_properties(windows.text.encoding_strict
                     windows.text.length_guards
                     windows.text.suite_can_fail
  PROPERTIES TIMEOUT 180
             LABELS "windows;text;utf8;utf16;encoding")

message(STATUS "  text_bridge cases    : 3 (encoding_strict, length_guards, "
               "suite_can_fail)")
