# EpoCanva P3.3 -- the Windows API adapter verification program and its CTest cases.
#
# NOT A STANDALONE PROJECT. This directory is included by platforms/windows/input/
# CMakeLists.txt. The guard below keeps it from being configured alone, where it would have
# no adapter library to judge and no CRT agreement with it.
#
# This program links the adapter library directly. It calls the real user32 APIs against the
# layouts that are already loaded on the machine; it never installs, loads or activates a
# layout. It is a separate executable from input_decision_tests on purpose: a green pure run
# must never be reported as a real-layout result, and a green run here says nothing about the
# pure rule table beyond the samples it feeds through it.
#
# Judging uses the repository's existing wrapper tests/english_completion/assert_exit.cmake.

if(NOT TARGET epocanva_keyboard_adapters)
  message(FATAL_ERROR
    "tests/windows/keyboard_translation must be included from platforms/windows/input (the "
    "top-level project that builds epocanva_keyboard_adapters); it is not configurable alone.")
endif()

add_executable(keyboard_translation_tests main.cc)
target_compile_definitions(keyboard_translation_tests PRIVATE
  _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_compile_options(keyboard_translation_tests PRIVATE
  /W4 /permissive- /EHsc /Zc:__cplusplus /utf-8)
target_link_libraries(keyboard_translation_tests PRIVATE epocanva_keyboard_adapters)

enable_testing()

set(_input_assert "${CMAKE_CURRENT_LIST_DIR}/../../english_completion/assert_exit.cmake")

# 1. The snapshot: a real GetKeyboardState capture, modifier derivation from controlled
#    256-byte arrays (not from the machine's live keys), byte-for-byte copying, a null HKL
#    refused without a call, and two snapshots remaining independent.
add_test(
  NAME windows.input.keyboard_snapshot
  COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:keyboard_translation_tests>
    -DCASE=keyboard-snapshot
    -DEXPECTED_EXIT=0
    "-DMARKER=RESULT PASS"
    -DMARKER_POLICY=require
    "-DPASS_ARGS=--mode;snapshot"
    -DTIMEOUT_SECONDS=120
    -P ${_input_assert}
)

# 2. The real translation: every schema character each installed layout can type without
#    Ctrl/Alt, 'a'/'A' explicitly, non-ASCII where a layout provides it, dead keys where a
#    layout has one (including the bit-2 non-mutation probe), a no-text key, multi-unit
#    results where present, key-up refusal, and repeat metadata not changing the text.
add_test(
  NAME windows.input.text_translation
  COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:keyboard_translation_tests>
    -DCASE=text-translation
    -DEXPECTED_EXIT=0
    "-DMARKER=RESULT PASS"
    -DMARKER_POLICY=require
    "-DPASS_ARGS=--mode;translation"
    -DTIMEOUT_SECONDS=180
    -P ${_input_assert}
)

# 3. The falsifiability control: one deliberately false claim (that a key-up is translated)
#    must exit 14 with no RESULT PASS.
add_test(
  NAME windows.input.translation_can_fail
  COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:keyboard_translation_tests>
    -DCASE=translation-can-fail
    -DEXPECTED_EXIT=14
    "-DMARKER=RESULT PASS"
    -DMARKER_POLICY=forbid
    "-DPASS_ARGS=--mode;self-check-fail"
    -DTIMEOUT_SECONDS=120
    -P ${_input_assert}
)

set_tests_properties(
                     windows.input.keyboard_snapshot
                     windows.input.text_translation
                     windows.input.translation_can_fail
  PROPERTIES TIMEOUT 240
             LABELS "windows;input;keyboard;translation;win32")

message(STATUS "  keyboard_translation cases : 3 (keyboard_snapshot, text_translation, "
               "translation_can_fail)")
