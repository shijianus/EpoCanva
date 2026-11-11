cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(EpoCanvaPredictionAbbreviationTests LANGUAGES CXX)
  enable_testing()
endif()

if(NOT TARGET epocanva_prediction_core)
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../engine/prediction"
                   "${CMAKE_CURRENT_BINARY_DIR}/prediction-core")
endif()
# Standalone fallback; the embedding build may own this production target.
if(NOT TARGET epocanva_prediction_abbreviation)
  add_library(epocanva_prediction_abbreviation STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/../../engine/prediction/abbreviation.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../engine/prediction/abbreviation.h")
  target_link_libraries(epocanva_prediction_abbreviation PUBLIC epocanva_prediction_core)
  target_compile_features(epocanva_prediction_abbreviation PUBLIC cxx_std_17)
  set_target_properties(epocanva_prediction_abbreviation PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON FOLDER "epocanva/prediction")
  if(MSVC)
    target_compile_options(epocanva_prediction_abbreviation PRIVATE
      /utf-8 /permissive- /Zc:__cplusplus /W4 /EHsc)
  else()
    target_compile_options(epocanva_prediction_abbreviation PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endif()

find_package(Threads REQUIRED)
add_executable(epocanva_prediction_abbreviation_tests main.cc)
target_link_libraries(epocanva_prediction_abbreviation_tests PRIVATE
  epocanva_prediction_abbreviation Threads::Threads)
set_target_properties(epocanva_prediction_abbreviation_tests PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
  FOLDER "epocanva/prediction")
if(MSVC)
  target_compile_options(epocanva_prediction_abbreviation_tests PRIVATE
    /utf-8 /permissive- /Zc:__cplusplus /W4 /EHsc)
else()
  target_compile_options(epocanva_prediction_abbreviation_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

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
  add_test(NAME prediction.abbreviation.${_case}
    COMMAND "${CMAKE_COMMAND}"
      "-DPROBE=$<TARGET_FILE:epocanva_prediction_abbreviation_tests>"
      "-DCASE=prediction-abbreviation-${_case}" "-DPROBE_ARG1=${_mode}"
      "-DEXPECTED_EXIT=${_exit}" "-DEXPECT_MARKER=${_marker}"
      "-DMARKER=RESULT PASS" -DTIMEOUT_SECONDS=120
      -P "${CMAKE_CURRENT_SOURCE_DIR}/../infrastructure/assert_probe.cmake")
  set_tests_properties(prediction.abbreviation.${_case} PROPERTIES
    TIMEOUT 150 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    LABELS "prediction;abbreviation;unit")
endforeach()
