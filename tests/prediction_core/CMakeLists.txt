cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(EpoCanvaPredictionCoreTests LANGUAGES CXX)
  enable_testing()
endif()

if(NOT TARGET epocanva_prediction_core)
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../engine/prediction"
                   "${CMAKE_CURRENT_BINARY_DIR}/prediction-core")
endif()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
find_package(Threads REQUIRED)
set(_prediction_fixtures "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
add_custom_command(
  OUTPUT "${_prediction_fixtures}/fixtures.stamp"
  COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/make_fixtures.py"
          "${_prediction_fixtures}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/make_fixtures.py"
  COMMENT "Generate isolated test-only prediction fixtures in the build tree"
  VERBATIM)
add_custom_target(epocanva_prediction_test_fixtures
  DEPENDS "${_prediction_fixtures}/fixtures.stamp")

add_executable(epocanva_prediction_core_tests main.cc)
target_link_libraries(epocanva_prediction_core_tests PRIVATE
  epocanva_prediction_core Threads::Threads)
add_dependencies(epocanva_prediction_core_tests epocanva_prediction_test_fixtures)
set_target_properties(epocanva_prediction_core_tests PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
  FOLDER "epocanva/prediction")
if(MSVC)
  target_compile_options(epocanva_prediction_core_tests PRIVATE
    /utf-8 /permissive- /Zc:__cplusplus /W4 /EHsc)
else()
  target_compile_options(epocanva_prediction_core_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

set(_prediction_assert "${CMAKE_CURRENT_SOURCE_DIR}/../infrastructure/assert_probe.cmake")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/run")
foreach(_case IN ITEMS unit forcefail bad_args)
  if(_case STREQUAL "unit")
    set(_mode --unit)
    set(_exit 0)
    set(_marker TRUE)
  elseif(_case STREQUAL "forcefail")
    set(_mode --forcefail)
    set(_exit 1)
    set(_marker FALSE)
  else()
    set(_mode --invalid-mode)
    set(_exit 2)
    set(_marker FALSE)
  endif()
  set(_fixture_arg)
  if(_case STREQUAL "unit")
    set(_fixture_arg "-DPROBE_ARG2=${_prediction_fixtures}")
  endif()
  add_test(NAME prediction.core.${_case}
    COMMAND "${CMAKE_COMMAND}"
      "-DPROBE=$<TARGET_FILE:epocanva_prediction_core_tests>"
      "-DCASE=prediction-${_case}" "-DPROBE_ARG1=${_mode}" ${_fixture_arg}
      "-DEXPECTED_EXIT=${_exit}" "-DEXPECT_MARKER=${_marker}"
      "-DMARKER=RESULT PASS" -DTIMEOUT_SECONDS=120
      -P "${_prediction_assert}")
  set_tests_properties(prediction.core.${_case} PROPERTIES
    TIMEOUT 150 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/run"
    LABELS "prediction;core;unit")
endforeach()
