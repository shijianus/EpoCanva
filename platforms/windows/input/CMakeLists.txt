# EpoCanva P3.3 stage 1 -- the pure keyboard decision layer.
#
# STANDALONE ON PURPOSE, following the P3.1/P3.2 precedent: its own top-level project, its own
# build directory (build/windows-input/stage1 for this stage), and no dependency on librime,
# the engine, third_party or even <windows.h>. The pure decision target compiles and is tested
# without a window station, a message queue or a keyboard, which is the point of the split: a
# green run here says the decision table is deterministic, not that a real layout was hit.
#
# WHAT IS BUILT AT THIS STAGE
#   epocanva_key_decisions       the pure rules (key_event.h, key_decision.{h,cc})
#   input_decision_tests         the pure verification program
#
# WHAT IS NOT BUILT YET: the Windows keyboard-state snapshot and the ToUnicodeEx adapter.
# They arrive in stage 2 with their own target and their own CTest cases; the build file is
# extended then rather than pretending they existed now.
#
# CRT: static (/MT), same as the two Windows projects before this one, so a later link into
# epocanva_tsf.dll does not mix two CRTs in one host process.
cmake_minimum_required(VERSION 3.25)
project(epocanva_windows_input VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  message(FATAL_ERROR
    "This project must be configured as its own top-level project so its CRT choice "
    "cannot be overridden by the EpoCanva root project.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT MSVC)
  message(FATAL_ERROR
    "The Windows input layer is built with the MSVC toolchain only: it is destined for a "
    "TSF DLL and its adapter calls Win32 keyboard APIs.")
endif()

if(NOT CMAKE_GENERATOR_PLATFORM STREQUAL "x64")
  message(WARNING
    "CMAKE_GENERATOR_PLATFORM is '${CMAKE_GENERATOR_PLATFORM}', not x64. The Windows input "
    "layer is only built for x64.")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

set(_input_compile_options /W4 /permissive- /EHsc /Zc:__cplusplus /utf-8)
set(_input_compile_definitions _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)

add_library(epocanva_key_decisions STATIC
  key_decision.cc
  key_decision.h
  key_event.h)
target_compile_definitions(epocanva_key_decisions PRIVATE ${_input_compile_definitions})
target_compile_options(epocanva_key_decisions PRIVATE ${_input_compile_options})
target_include_directories(epocanva_key_decisions PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# --- the verification program ---------------------------------------------------------
# Included from this project so one configure builds the library and the program that judges
# it. The subdirectory refuses to be configured on its own -- see its guard.
add_subdirectory(
  "${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/windows/input_decision"
  "${CMAKE_CURRENT_BINARY_DIR}/input_decision")

enable_testing()

message(STATUS "epocanva_windows_input summary")
message(STATUS "  generator            : ${CMAKE_GENERATOR}")
message(STATUS "  platform             : ${CMAKE_GENERATOR_PLATFORM}")
message(STATUS "  configuration types  : ${CMAKE_CONFIGURATION_TYPES}")
message(STATUS "  MSVC runtime library : ${CMAKE_MSVC_RUNTIME_LIBRARY} (static CRT)")
message(STATUS "  stage                : 1 (pure decisions only)")
message(STATUS "  system API used      : none in this stage")
message(STATUS "  registration         : none -- no DLL target, no registry write")
message(STATUS "  engine/third_party   : not referenced by any target here")
