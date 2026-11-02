cmake_minimum_required(VERSION 3.25)

# Standalone verification is the primary entry point; add_subdirectory mode reuses
# an already-defined session target and does not alter the parent project's CRT.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(EpoCanvaBackendSessionTests LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  enable_testing()
endif()

get_filename_component(_backend_session_repo "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
if(NOT TARGET epocanva_backend_session)
  add_subdirectory("${_backend_session_repo}/engine/backend_session"
                   "${CMAKE_CURRENT_BINARY_DIR}/backend_session_core")
endif()

add_executable(epocanva_backend_session_tests main.cc)
target_link_libraries(epocanva_backend_session_tests PRIVATE epocanva_backend_session)
target_compile_features(epocanva_backend_session_tests PRIVATE cxx_std_17)
set_target_properties(epocanva_backend_session_tests PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
if(MSVC)
  target_compile_options(epocanva_backend_session_tests PRIVATE
    /utf-8 /permissive- /W4 /WX /EHsc)
else()
  target_compile_options(epocanva_backend_session_tests PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
endif()

set(_backend_session_assert
    "${_backend_session_repo}/tests/infrastructure/assert_probe.cmake")
if(NOT EXISTS "${_backend_session_assert}")
  message(FATAL_ERROR "missing assertion wrapper: ${_backend_session_assert}")
endif()

foreach(_case IN ITEMS protocol context privacy validation catalogs reset failure wrong_thread forcefail)
  if(_case STREQUAL "forcefail")
    set(_expected_exit 1)
    set(_expected_marker FALSE)
  else()
    set(_expected_exit 0)
    set(_expected_marker TRUE)
  endif()
  add_test(NAME "backend_session.${_case}"
    COMMAND "${CMAKE_COMMAND}"
      "-DPROBE=$<TARGET_FILE:epocanva_backend_session_tests>"
      "-DCASE=${_case}"
      "-DPROBE_ARG1=${_case}"
      "-DEXPECTED_EXIT=${_expected_exit}"
      "-DEXPECT_MARKER=${_expected_marker}"
      "-DMARKER=RESULT PASS"
      "-DTIMEOUT_SECONDS=60"
      -P "${_backend_session_assert}")
  set_tests_properties("backend_session.${_case}" PROPERTIES
    TIMEOUT 90 LABELS "backend-session;unit")
endforeach()
