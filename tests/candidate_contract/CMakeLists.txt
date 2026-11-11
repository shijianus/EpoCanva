cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(EpoCanvaCandidateContractTests LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  enable_testing()
endif()

get_filename_component(_candidate_repo "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(_candidate_build_root "${_candidate_repo}/build")
cmake_path(IS_PREFIX _candidate_build_root "${CMAKE_BINARY_DIR}" NORMALIZE _candidate_in_build)
if(NOT _candidate_in_build OR CMAKE_BINARY_DIR STREQUAL _candidate_build_root)
  message(FATAL_ERROR "Candidate contract test artifacts must stay in build/<preset>/")
endif()
if(NOT TARGET epocanva_candidates)
  add_subdirectory("${_candidate_repo}/engine/candidates" "${CMAKE_CURRENT_BINARY_DIR}/candidates_core")
endif()

add_executable(epocanva_candidate_contract_tests main.cc)
target_link_libraries(epocanva_candidate_contract_tests PRIVATE epocanva_candidates epocanva_strict)
target_include_directories(epocanva_candidate_contract_tests PRIVATE
  "${_candidate_repo}/engine/include")
set_target_properties(epocanva_candidate_contract_tests PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

foreach(_case unit forcefail unknown)
  if(_case STREQUAL "unit")
    set(_expected_exit 0)
    set(_expect_marker TRUE)
  elseif(_case STREQUAL "forcefail")
    set(_expected_exit 1)
    set(_expect_marker FALSE)
  else()
    set(_expected_exit 2)
    set(_expect_marker FALSE)
  endif()
  add_test(NAME candidate_contract.${_case}
    COMMAND "${CMAKE_COMMAND}"
      -DPROBE=$<TARGET_FILE:epocanva_candidate_contract_tests>
      -DCASE=${_case} -DPROBE_ARG1=--${_case}
      -DEXPECTED_EXIT=${_expected_exit} -DEXPECT_MARKER=${_expect_marker}
      "-DMARKER=RESULT PASS" -DTIMEOUT_SECONDS=60
      -P "${_candidate_repo}/tests/infrastructure/assert_probe.cmake")
  set_tests_properties(candidate_contract.${_case} PROPERTIES
    TIMEOUT 90 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    LABELS "candidates;core;unit;contract-gate")
endforeach()
