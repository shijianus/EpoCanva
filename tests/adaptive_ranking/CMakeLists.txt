cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(EpoCanvaAdaptiveRankingTests LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  enable_testing()
endif()

get_filename_component(_ranking_repo "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(_ranking_build_root "${_ranking_repo}/build")
cmake_path(IS_PREFIX _ranking_build_root "${CMAKE_BINARY_DIR}" NORMALIZE _ranking_in_build)
if(NOT _ranking_in_build OR CMAKE_BINARY_DIR STREQUAL _ranking_build_root)
  message(FATAL_ERROR "Adaptive ranking test artifacts must stay in build/<preset>/")
endif()
if(NOT TARGET epocanva_adaptive_ranking)
  add_subdirectory("${_ranking_repo}/engine/ranking" "${CMAKE_CURRENT_BINARY_DIR}/ranking_core")
endif()

add_executable(epocanva_adaptive_ranking_tests main.cc)
target_link_libraries(epocanva_adaptive_ranking_tests
  PRIVATE epocanva_adaptive_ranking epocanva_strict)
set_target_properties(epocanva_adaptive_ranking_tests PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

# One CTest entry per behaviour so a regression names what broke.
set(_ranking_cases
  prefix
  ordering
  no_model
  learning
  persistence
  isolation
  negative
  decay
  rejected_events
  clear
  mixed_language
  detection_latch
  bounds
  spelling)
foreach(_case IN LISTS _ranking_cases)
  add_test(NAME adaptive_ranking.${_case}
    COMMAND "${CMAKE_COMMAND}"
      -DPROBE=$<TARGET_FILE:epocanva_adaptive_ranking_tests>
      -DCASE=${_case} -DPROBE_ARG1=--${_case}
      -DEXPECTED_EXIT=0 -DEXPECT_MARKER=TRUE
      "-DMARKER=RESULT PASS" -DTIMEOUT_SECONDS=60
      -P "${_ranking_repo}/tests/infrastructure/assert_probe.cmake")
  set_tests_properties(adaptive_ranking.${_case} PROPERTIES
    TIMEOUT 90 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    LABELS "ranking;core;unit")
endforeach()

# A run that reports every case green must not be taken on faith: the same
# binary must be able to fail.
add_test(NAME adaptive_ranking.can_fail
  COMMAND "${CMAKE_COMMAND}"
    -DPROBE=$<TARGET_FILE:epocanva_adaptive_ranking_tests>
    -DCASE=can_fail -DPROBE_ARG1=--forcefail
    -DEXPECTED_EXIT=1 -DEXPECT_MARKER=FALSE
    "-DMARKER=RESULT PASS" -DTIMEOUT_SECONDS=60
    -P "${_ranking_repo}/tests/infrastructure/assert_probe.cmake")
set_tests_properties(adaptive_ranking.can_fail PROPERTIES
  TIMEOUT 90 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
  LABELS "ranking;core;negative")
