# EpoCanva P3.2 -- the Windows text encoding adapter layer.
#
# STANDALONE ON PURPOSE, following the precedent platforms/windows/tsf set in P3.1: this is
# its own top-level project, configured with the VS 2022 generator into its own build
# directory (build/windows-text/), because
#   - it needs nothing but the Windows SDK: no librime, no engine sources, no third_party,
#     so this round's build cannot drag the engine's artefacts back into existence;
#   - the CRT choice must match the DLL this layer is destined to be linked into. The root
#     EpoCanva project is Debug / dynamic CRT; a normal adapter compiled there could not be
#     linked into a TSF DLL that carries a static CRT.
#
# WHAT IS BUILT HERE
#   epocanva_text_encoding        the strict UTF-8 <-> UTF-16 conversions (text_encoding.*)
#   text_bridge                   the verification program from
#                                 tests/windows/text_bridge, with its named CTest cases
#
# WHAT IS NOT BUILT HERE: no DLL, no registration, no key sink, no composition, no candidate
# UI, no engine session, no host text editing. Nothing in this directory edits anything.
#
# CRT: static (/MT), same as platforms/windows/tsf. Both libraries and the test program are
# built with it, so a later link into epocanva_tsf.dll does not mix two CRTs in one process.
cmake_minimum_required(VERSION 3.25)
project(epocanva_windows_text VERSION 0.1.0 LANGUAGES CXX)

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
    "The Windows text adapter is built with the MSVC toolchain only: its converter IS a "
    "Win32 API (MultiByteToWideChar / WideCharToMultiByte with CP_UTF8).")
endif()

if(NOT CMAKE_GENERATOR_PLATFORM STREQUAL "x64")
  message(WARNING
    "CMAKE_GENERATOR_PLATFORM is '${CMAKE_GENERATOR_PLATFORM}', not x64. The Windows "
    "adapter layer is only built for x64.")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

add_library(epocanva_text_encoding STATIC
  text_encoding.cc
  text_encoding.h)
target_compile_definitions(epocanva_text_encoding PRIVATE
  _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_compile_options(epocanva_text_encoding PRIVATE
  /W4 /permissive- /EHsc /Zc:__cplusplus /utf-8)
# PUBLIC: the headers state the unit contract, so every consumer compiles against the same
# declarations and cannot invent an off-by-a-unit copy of them.
target_include_directories(epocanva_text_encoding PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# --- the verification program ---------------------------------------------------------
# Included from this project so one configure builds the libraries and the tests, and so the
# test target can depend on the libraries it judges. The subdirectory refuses to be
# configured on its own -- see its guard.
add_subdirectory(
  "${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/windows/text_bridge"
  "${CMAKE_CURRENT_BINARY_DIR}/text_bridge")

enable_testing()

message(STATUS "epocanva_windows_text summary")
message(STATUS "  generator            : ${CMAKE_GENERATOR}")
message(STATUS "  platform             : ${CMAKE_GENERATOR_PLATFORM}")
message(STATUS "  configuration types  : ${CMAKE_CONFIGURATION_TYPES}")
message(STATUS "  MSVC runtime library : ${CMAKE_MSVC_RUNTIME_LIBRARY} (static CRT)")
message(STATUS "  system API used      : kernel32 MultiByteToWideChar / WideCharToMultiByte")
message(STATUS "  registration         : none -- no DLL target, no registry write")
message(STATUS "  engine/third_party   : not referenced by any target here")
