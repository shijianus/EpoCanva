# Standalone: cmake -S tests/composition_core -B build/multi-composition-core ...
# Also usable via add_subdirectory; never changes parent CRT or product targets.
cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(EpoCanvaMultilingualCompositionCore LANGUAGES CXX)
  enable_testing()
endif()

get_filename_component(EPOCANVA_COMPOSITION_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_library(epocanva_ja_composition_core STATIC
  "${EPOCANVA_COMPOSITION_ROOT}/engine/languages/ja/romaji_composer.cc"
  "${EPOCANVA_COMPOSITION_ROOT}/engine/languages/ja/romaji_composer.h"
)
target_include_directories(epocanva_ja_composition_core PUBLIC
  "${EPOCANVA_COMPOSITION_ROOT}/engine/languages/ja")

add_library(epocanva_ko_composition_core STATIC
  "${EPOCANVA_COMPOSITION_ROOT}/engine/languages/ko/two_set_composer.cc"
  "${EPOCANVA_COMPOSITION_ROOT}/engine/languages/ko/two_set_composer.h"
)
target_include_directories(epocanva_ko_composition_core PUBLIC
  "${EPOCANVA_COMPOSITION_ROOT}/engine/languages/ko")

add_executable(epocanva_multilingual_composition_tests
  main.cc japanese_tests.cc korean_tests.cc invariant_tests.cc test_support.h)
target_link_libraries(epocanva_multilingual_composition_tests PRIVATE
  epocanva_ja_composition_core epocanva_ko_composition_core)

foreach(composition_target IN ITEMS epocanva_ja_composition_core epocanva_ko_composition_core
                                    epocanva_multilingual_composition_tests)
  set_target_properties(${composition_target} PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  if(MSVC)
    target_compile_options(${composition_target} PRIVATE /permissive- /utf-8 /W4 /WX)
  else()
    target_compile_options(${composition_target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endforeach()

function(epocanva_add_composition_test case_name probe_arg expected_exit control)
  add_test(NAME "composition.core.${case_name}"
    COMMAND "${CMAKE_COMMAND}"
      "-DPROBE=$<TARGET_FILE:epocanva_multilingual_composition_tests>"
      "-DPROBE_ARG=${probe_arg}"
      "-DEXPECTED_EXIT=${expected_exit}"
      "-DCONTROL=${control}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/assert_result.cmake")
  set_tests_properties("composition.core.${case_name}" PROPERTIES
    TIMEOUT 150
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    LABELS "composition;core;offline")
endfunction()

epocanva_add_composition_test(japanese --japanese 0 normal)
epocanva_add_composition_test(korean --korean 0 normal)
epocanva_add_composition_test(invariants --invariants 0 normal)
epocanva_add_composition_test(intentional_failure --force-fail 1 intentional)
epocanva_add_composition_test(rejects_unknown_argument --not-a-suite 2 arguments)
