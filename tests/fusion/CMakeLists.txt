cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(EpoCanvaFusionTests LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  enable_testing()
endif()

get_filename_component(_fusion_repo "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(_fusion_build_root "${_fusion_repo}/build")
cmake_path(IS_PREFIX _fusion_build_root "${CMAKE_BINARY_DIR}" NORMALIZE _fusion_in_build)
if(NOT _fusion_in_build OR CMAKE_BINARY_DIR STREQUAL _fusion_build_root)
  message(FATAL_ERROR "Fusion test artifacts must stay in build/<preset>/")
endif()
if(NOT TARGET epocanva_fusion)
  add_subdirectory("${_fusion_repo}/engine/fusion" "${CMAKE_CURRENT_BINARY_DIR}/fusion_core")
endif()
if(NOT TARGET epocanva_adaptive_ranking)
  add_subdirectory("${_fusion_repo}/engine/ranking" "${CMAKE_CURRENT_BINARY_DIR}/ranking_core")
endif()

add_executable(epocanva_fusion_tests main.cc)
target_link_libraries(epocanva_fusion_tests PRIVATE epocanva_fusion epocanva_adaptive_ranking)
target_include_directories(epocanva_fusion_tests PRIVATE "${_fusion_repo}")
target_compile_definitions(epocanva_fusion_tests PRIVATE
  "EPOCANVA_FUSION_POLICY_JSON=\"${_fusion_repo}/data/eval/candidate-v1/fusion-policy.json\"")
target_compile_features(epocanva_fusion_tests PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(epocanva_fusion_tests PRIVATE /utf-8 /permissive- /W4 /EHsc)
endif()
set_target_properties(epocanva_fusion_tests PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

# One CTest entry per behaviour so a regression names what broke.
set(_fusion_cases policy dedup merge mixed)
foreach(_case IN LISTS _fusion_cases)
  add_test(NAME fusion.${_case}
    COMMAND "${CMAKE_COMMAND}"
      -DPROBE=$<TARGET_FILE:epocanva_fusion_tests>
      -DCASE=${_case} -DPROBE_ARG1=--${_case}
      -DEXPECTED_EXIT=0 -DEXPECT_MARKER=TRUE
      "-DMARKER=RESULT PASS" -DTIMEOUT_SECONDS=60
      -P "${_fusion_repo}/tests/infrastructure/assert_probe.cmake")
  set_tests_properties(fusion.${_case} PROPERTIES
    TIMEOUT 90 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    LABELS "fusion;core;unit;contract-gate")
endforeach()

# A green run must be provable: the same binary must fail on demand.
add_test(NAME fusion.can_fail
  COMMAND "${CMAKE_COMMAND}"
    -DPROBE=$<TARGET_FILE:epocanva_fusion_tests>
    -DCASE=can_fail -DPROBE_ARG1=--forcefail
    -DEXPECTED_EXIT=1 -DEXPECT_MARKER=FALSE
    "-DMARKER=RESULT PASS" -DTIMEOUT_SECONDS=60
    -P "${_fusion_repo}/tests/infrastructure/assert_probe.cmake")
set_tests_properties(fusion.can_fail PROPERTIES
  TIMEOUT 90 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
  LABELS "fusion;core;negative;contract-gate")
