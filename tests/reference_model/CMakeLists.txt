# EpoCanva reference-model stress protocol (100,000 edits x 5 fixed seeds).
#
# Standalone: cmake -S tests/reference_model -B build/reference-model
# Also included by the root project and the Windows product build.
cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(EpoCanvaReferenceModel LANGUAGES CXX)
  enable_testing()
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  add_library(epocanva_strict INTERFACE)
  if(MSVC)
    target_compile_options(epocanva_strict INTERFACE
      /utf-8 /permissive- /Zc:__cplusplus /W4 /EHsc)
  else()
    target_compile_options(epocanva_strict INTERFACE -Wall -Wextra -Wpedantic)
  endif()
endif()

get_filename_component(EPOCANVA_REFERENCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_executable(epocanva_reference_model_stress_tests
  main.cc
  "${EPOCANVA_REFERENCE_ROOT}/engine/languages/ja/romaji_composer.cc"
  "${EPOCANVA_REFERENCE_ROOT}/engine/languages/ko/two_set_composer.cc"
  "${EPOCANVA_REFERENCE_ROOT}/platforms/windows/tsf_edit/edit_plan.cc"
  "${EPOCANVA_REFERENCE_ROOT}/platforms/windows/text/text_encoding.cc")
target_include_directories(epocanva_reference_model_stress_tests PRIVATE
  "${EPOCANVA_REFERENCE_ROOT}/engine/languages/ja"
  "${EPOCANVA_REFERENCE_ROOT}/engine/languages/ko"
  "${EPOCANVA_REFERENCE_ROOT}/platforms/windows/tsf_edit"
  "${EPOCANVA_REFERENCE_ROOT}/platforms/windows/text"
  "${EPOCANVA_REFERENCE_ROOT}/tests/composition_core")
target_compile_definitions(epocanva_reference_model_stress_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_link_libraries(epocanva_reference_model_stress_tests PRIVATE
  epocanva_strict psapi)
set_target_properties(epocanva_reference_model_stress_tests PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
  FOLDER "epocanva")

enable_testing()
add_test(
  NAME composition.core.reference_model_stress
  COMMAND epocanva_reference_model_stress_tests --ops 100000
    --report "${CMAKE_CURRENT_BINARY_DIR}/reference-model-report.json")
set_tests_properties(composition.core.reference_model_stress PROPERTIES
  TIMEOUT 1800
  LABELS "composition;core;offline;stress;reference-model")

# Negative control: a deliberately diverged model must be detected. The probe
# must exit 1 and must not print the success marker.
add_test(
  NAME composition.core.reference_model_can_fail
  COMMAND "${CMAKE_COMMAND}"
    -DPROBE=$<TARGET_FILE:epocanva_reference_model_stress_tests>
    -DPROBE_ARG1=--force-fail
    -DEXPECTED_EXIT=1
    -DEXPECT_MARKER=FALSE
    "-DMARKER=RESULT PASS"
    -DCASE=reference-model-can-fail
    -DTIMEOUT_SECONDS=60
    -P "${EPOCANVA_REFERENCE_ROOT}/tests/infrastructure/assert_probe.cmake")
set_tests_properties(composition.core.reference_model_can_fail PROPERTIES
  TIMEOUT 120
  LABELS "composition;core;offline;negative;reference-model")
