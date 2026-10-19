# EpoCanva P1.2 -- standalone project for the minimal librime lifecycle check.
#
# It is deliberately NOT add_subdirectory()'d into the root EpoCanva project: the
# root project is a Debug / dynamic-CRT probe, while this program must link the
# Release / static-CRT librime import library built by
# tools/build_librime_baseline.ps1. Keeping them separate stops one CRT from
# silently leaking into the other, and keeps the third-party build chain out of
# the repository's own configure step.
#
# Configure with:
#   cmake -S tools/rime_lifecycle_smoke -B build/librime-baseline/smoke \
#         -DRime_Install_Prefix=<repo>/build/librime-baseline/rime/install

cmake_minimum_required(VERSION 3.25)
project(epocanva_rime_lifecycle_smoke VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  message(FATAL_ERROR
    "This project must be configured as its own top-level project so that its CRT "
    "setting cannot be overridden by the EpoCanva root project.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT MSVC)
  message(FATAL_ERROR
    "The P1.2 baseline is a Windows/MSVC artefact; this project links its import library.")
endif()

# Must match the CRT of rime.lib and of every static dependency folded into it.
# Declared here rather than on the command line so a stale cache cannot quietly
# leave it at the default /MD. Policy CMP0091 is NEW at cmake_minimum_required 3.25,
# so this variable -- not a make-rules override -- is what selects the switch.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

set(Rime_Install_Prefix "" CACHE PATH
  "Installation prefix produced by tools/build_librime_baseline.ps1 (the directory holding bin/, lib/, include/)")
if(NOT Rime_Install_Prefix)
  message(FATAL_ERROR
    "Rime_Install_Prefix is required. Point it at build/librime-baseline/rime/install.")
endif()
get_filename_component(Rime_Install_Prefix "${Rime_Install_Prefix}" ABSOLUTE)
if(NOT IS_DIRECTORY "${Rime_Install_Prefix}/include")
  message(FATAL_ERROR "no include/ under Rime_Install_Prefix: ${Rime_Install_Prefix}")
endif()

# Upstream installs the Windows DLL next to its import library (lib/), while the
# rime_*.exe tools go to bin/; search both, but never outside the given prefix.
find_file(Rime_Api_Header rime_api.h
  PATHS "${Rime_Install_Prefix}/include"
  NO_DEFAULT_PATH
  REQUIRED)
find_library(Rime_Import_Library rime
  PATHS "${Rime_Install_Prefix}/lib"
  NO_DEFAULT_PATH
  REQUIRED)
find_file(Rime_Shared_Library rime.dll
  PATHS "${Rime_Install_Prefix}/lib" "${Rime_Install_Prefix}/bin"
  NO_DEFAULT_PATH
  REQUIRED)

# NO_DEFAULT_PATH above is what keeps this project from silently linking a librime
# installed elsewhere on the machine.
file(TO_CMAKE_PATH "${Rime_Install_Prefix}" _norm_prefix)
foreach(_found Rime_Api_Header Rime_Import_Library Rime_Shared_Library)
  file(TO_CMAKE_PATH "${${_found}}" _norm_found)
  string(FIND "${_norm_found}" "${_norm_prefix}/" _prefix_offset)
  if(NOT _prefix_offset EQUAL 0)
    message(FATAL_ERROR
      "${_found} resolved to ${${_found}}, which is not under ${Rime_Install_Prefix}")
  endif()
  message(STATUS "  ${_found} = ${${_found}}")
endforeach()

add_executable(rime_lifecycle_smoke main.cc)
target_compile_definitions(rime_lifecycle_smoke PRIVATE
  RIME_IMPORTS            # rime_api.h:23-26 -- dllimport view of the API
  _CRT_SECURE_NO_WARNINGS
  NOMINMAX
  WIN32_LEAN_AND_MEAN)
# Same strictness the root EpoCanva project applies to its own C++ targets.
target_compile_options(rime_lifecycle_smoke PRIVATE
  /utf-8 /permissive- /Zc:__cplusplus /Zc:preprocessor /W4 /EHsc)
target_include_directories(rime_lifecycle_smoke PRIVATE
  "${Rime_Install_Prefix}/include")
target_link_libraries(rime_lifecycle_smoke PRIVATE "${Rime_Import_Library}")

# Windows resolves an implicitly loaded DLL from the executable's own directory
# first, so placing the exact built rime.dll there makes the load path explicit and
# independent of PATH. The program then verifies at runtime that the module which
# supplied rime_get_api() really is this file.
add_custom_command(TARGET rime_lifecycle_smoke POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${Rime_Shared_Library}"
    "$<TARGET_FILE_DIR:rime_lifecycle_smoke>/rime.dll"
  COMMENT "Placing the built rime.dll next to the lifecycle test")

enable_testing()

set(_smoke_data_dir "${CMAKE_CURRENT_BINARY_DIR}/rime-lifecycle-data")

add_test(
  NAME librime.lifecycle_smoke.initialize_create_destroy_finalize
  COMMAND rime_lifecycle_smoke
    --expect-dll "$<TARGET_FILE_DIR:rime_lifecycle_smoke>/rime.dll"
    --shared-dir "${_smoke_data_dir}/shared"
    --user-dir "${_smoke_data_dir}/user"
    --log-dir "${_smoke_data_dir}/log"
)

# Exit status alone decides the result: no PASS_REGULAR_EXPRESSION / WILL_FAIL is
# used, because both can turn a non-zero exit into a green test. The program's own
# stage codes identify which step broke.
set_tests_properties(librime.lifecycle_smoke.initialize_create_destroy_finalize
  PROPERTIES
    TIMEOUT 120
    LABELS "librime;lifecycle")

message(STATUS "epocanva_rime_lifecycle_smoke summary")
message(STATUS "  generator              : ${CMAKE_GENERATOR}")
message(STATUS "  configuration types    : ${CMAKE_CONFIGURATION_TYPES}")
message(STATUS "  MSVC runtime library   : ${CMAKE_MSVC_RUNTIME_LIBRARY}")
message(STATUS "  rime install prefix    : ${Rime_Install_Prefix}")
message(STATUS "  test data directory    : ${_smoke_data_dir}")
