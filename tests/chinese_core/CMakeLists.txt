cmake_minimum_required(VERSION 3.21)

# Independent phonetic-only build; equally usable via add_subdirectory().
# Never configures Rime or changes a parent's runtime-library/CRT selection.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(EpoCanvaChineseCore LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  enable_testing()
endif()

get_filename_component(_zh_repo "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if(NOT TARGET epocanva_zh_core)
  add_library(epocanva_zh_core STATIC
    "${_zh_repo}/engine/languages/zh/phonetic_core.cc"
    "${_zh_repo}/engine/languages/zh/phonetic_core.h")
  target_include_directories(epocanva_zh_core PUBLIC "${_zh_repo}")
  target_compile_features(epocanva_zh_core PUBLIC cxx_std_17)
  set_target_properties(epocanva_zh_core PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  if(MSVC)
    target_compile_options(epocanva_zh_core PRIVATE /W4 /WX /permissive- /utf-8)
  else()
    target_compile_options(epocanva_zh_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endif()

add_executable(epocanva_chinese_core_tests main.cc)
target_link_libraries(epocanva_chinese_core_tests PRIVATE epocanva_zh_core)
set_target_properties(epocanva_chinese_core_tests PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
if(MSVC)
  target_compile_options(epocanva_chinese_core_tests PRIVATE /W4 /WX /permissive- /utf-8)
else()
  target_compile_options(epocanva_chinese_core_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

function(zh_core_test name mode exit_code expect_marker)
  add_test(NAME "chinese.core.${name}"
    COMMAND "${CMAKE_COMMAND}"
      "-DPROBE=$<TARGET_FILE:epocanva_chinese_core_tests>"
      "-DPROBE_ARG1=${mode}"
      "-DCASE=chinese_${name}"
      "-DEXPECTED_EXIT=${exit_code}"
      "-DEXPECT_MARKER=${expect_marker}"
      "-DMARKER=RESULT PASS"
      -DTIMEOUT_SECONDS=60
      -P "${_zh_repo}/tests/infrastructure/assert_probe.cmake")
  set_tests_properties("chinese.core.${name}" PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    TIMEOUT 90 LABELS "chinese;phonetic;core")
endfunction()

zh_core_test(pinyin_grammar --pinyin 0 TRUE)
zh_core_test(segmentation_ambiguity --segmentation 0 TRUE)
zh_core_test(zhuyin_state --zhuyin 0 TRUE)
zh_core_test(composition_edits --composition 0 TRUE)
zh_core_test(bounds_utf8_protection --bounds 0 TRUE)
zh_core_test(caller_dictionary --dictionary 0 TRUE)
zh_core_test(assertions_can_fail --forcefail 1 FALSE)
zh_core_test(rejects_unknown_arguments --unknown 2 FALSE)
