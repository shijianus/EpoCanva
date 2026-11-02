# May be configured standalone or included by an aggregate test project.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.20)
  project(epocanva_french_core_tests LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "Use an isolated build directory under build/<preset>.")
  endif()
  enable_testing()
endif()
get_filename_component(EPOCANVA_FR_REPO "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  file(REAL_PATH "${EPOCANVA_FR_REPO}/build" EPOCANVA_FR_BUILD_ROOT)
  file(REAL_PATH "${CMAKE_BINARY_DIR}" EPOCANVA_FR_BUILD_DIRECTORY)
  string(FIND "${EPOCANVA_FR_BUILD_DIRECTORY}/" "${EPOCANVA_FR_BUILD_ROOT}/" EPOCANVA_FR_BUILD_PREFIX)
  if(NOT EPOCANVA_FR_BUILD_PREFIX EQUAL 0)
    message(FATAL_ERROR "French core build artifacts must stay under the repository build/ tree.")
  endif()
endif()

add_library(epocanva_fr_core STATIC
  "${EPOCANVA_FR_REPO}/engine/languages/fr/french_core.cc")
target_include_directories(epocanva_fr_core PUBLIC
  "${EPOCANVA_FR_REPO}/engine/languages/fr")
add_executable(epocanva_french_core_tests main.cc)
target_link_libraries(epocanva_french_core_tests PRIVATE epocanva_fr_core)
foreach(target epocanva_fr_core epocanva_french_core_tests)
  set_target_properties(${target} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /WX /permissive- /utf-8)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endforeach()

foreach(case utf8 accents_case tokens composition suggestions plurals properties)
  add_test(NAME french.core.${case}
    COMMAND "${CMAKE_COMMAND}"
      "-DPROBE=$<TARGET_FILE:epocanva_french_core_tests>"
      "-DMODE=${case}" -DEXPECTED_EXIT=0 "-DEXPECTED_MARKER=RESULT PASS"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/assert_exit.cmake")
  set_tests_properties(french.core.${case} PROPERTIES
    TIMEOUT 30 WORKING_DIRECTORY "${CMAKE_BINARY_DIR}" LABELS "french;core;unit")
endforeach()
add_test(NAME french.core.assertions_can_fail
  COMMAND "${CMAKE_COMMAND}"
    "-DPROBE=$<TARGET_FILE:epocanva_french_core_tests>"
    -DMODE=force_failure -DEXPECTED_EXIT=1 "-DEXPECTED_MARKER=RESULT FAIL"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/assert_exit.cmake")
add_test(NAME french.core.rejects_unknown_mode
  COMMAND "${CMAKE_COMMAND}"
    "-DPROBE=$<TARGET_FILE:epocanva_french_core_tests>"
    -DMODE=not_a_mode -DEXPECTED_EXIT=2 "-DEXPECTED_MARKER=INVALID MODE"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/assert_exit.cmake")
set_tests_properties(french.core.assertions_can_fail french.core.rejects_unknown_mode
  PROPERTIES TIMEOUT 30 WORKING_DIRECTORY "${CMAKE_BINARY_DIR}" LABELS "french;core;negative")
