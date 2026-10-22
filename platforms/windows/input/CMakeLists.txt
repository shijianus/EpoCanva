# EpoCanva P3.3 -- the Windows keyboard input layer.
#
# STANDALONE ON PURPOSE, following the P3.1/P3.2 precedent: its own top-level project, its own
# build directory (build/windows-input/test), no librime, no engine and no third_party. The
# pure decision target still compiles without <windows.h>; only the adapter targets in this
# stage touch Win32. Keeping them separate is what lets a green pure run be reported as a
# statement about the rules, never as a statement about a real keyboard layout.
#
# WHAT IS BUILT HERE
#   epocanva_key_decisions            the pure rules (key_event.h, key_decision.{h,cc})
#   epocanva_input_text_encoding      P3.2's strict UTF-8 <-> UTF-16 conversion, compiled from
#                                     ../text/text_encoding.cc -- the SAME source file, not a
#                                     copy of its behaviour. P3.2's own top-level project
#                                     remains its canonical build; this project must not be
#                                     linked into the same binary as that one, or the two
#                                     copies of the conversion symbols would collide.
#   epocanva_keyboard_adapters        the 256-byte snapshot (keyboard_state.{h,cc}) and the
#                                     non-mutating ToUnicodeEx translation
#                                     (key_text_translation.{h,cc})
#   input_decision_tests              the pure verification program
#   keyboard_translation_tests        the real-API verification program
#
# WHAT IS NOT BUILT HERE: no DLL, no key sink, no AdviseKeyEventSink, no engine session, no
# composition, no candidate UI, no host text edit, no registration. Nothing in this directory
# inserts a character or sends an input message.
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

# --- the pure decision layer ----------------------------------------------------------
add_library(epocanva_key_decisions STATIC
  key_decision.cc
  key_decision.h
  key_event.h)
target_compile_definitions(epocanva_key_decisions PRIVATE ${_input_compile_definitions})
target_compile_options(epocanva_key_decisions PRIVATE ${_input_compile_options})
target_include_directories(epocanva_key_decisions PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# --- P3.2's strict conversion, from its one source file -------------------------------
add_library(epocanva_input_text_encoding STATIC
  "${CMAKE_CURRENT_SOURCE_DIR}/../text/text_encoding.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/../text/text_encoding.h")
target_compile_definitions(epocanva_input_text_encoding PRIVATE
  ${_input_compile_definitions} UNICODE _UNICODE)
target_compile_options(epocanva_input_text_encoding PRIVATE ${_input_compile_options})
target_include_directories(epocanva_input_text_encoding PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/../text")

# --- the Windows adapters -------------------------------------------------------------
add_library(epocanva_keyboard_adapters STATIC
  keyboard_state.cc
  keyboard_state.h
  key_text_translation.cc
  key_text_translation.h)
target_compile_definitions(epocanva_keyboard_adapters PRIVATE
  ${_input_compile_definitions} UNICODE _UNICODE)
target_compile_options(epocanva_keyboard_adapters PRIVATE ${_input_compile_options})
target_include_directories(epocanva_keyboard_adapters PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
# PUBLIC: the adapter's own headers expose KeyText/KeyEventSnapshot to its test.
# PRIVATE: the encoding library is an implementation detail; user32 supplies
# GetKeyboardState, GetKeyboardLayout and ToUnicodeEx.
target_link_libraries(epocanva_keyboard_adapters
  PUBLIC epocanva_key_decisions
  PRIVATE epocanva_input_text_encoding user32)

# --- the verification programs --------------------------------------------------------
# Included from this project so one configure builds the libraries and the programs that
# judge them. Each subdirectory refuses to be configured on its own -- see its guard.
add_subdirectory(
  "${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/windows/input_decision"
  "${CMAKE_CURRENT_BINARY_DIR}/input_decision")
add_subdirectory(
  "${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/windows/keyboard_translation"
  "${CMAKE_CURRENT_BINARY_DIR}/keyboard_translation")

enable_testing()

message(STATUS "epocanva_windows_input summary")
message(STATUS "  generator            : ${CMAKE_GENERATOR}")
message(STATUS "  platform             : ${CMAKE_GENERATOR_PLATFORM}")
message(STATUS "  configuration types  : ${CMAKE_CONFIGURATION_TYPES}")
message(STATUS "  MSVC runtime library : ${CMAKE_MSVC_RUNTIME_LIBRARY} (static CRT)")
message(STATUS "  system API used      : user32 GetKeyboardState / GetKeyboardLayout / "
               "ToUnicodeEx (bit 2, no state change)")
message(STATUS "  registration         : none -- no DLL target, no registry write")
message(STATUS "  engine/third_party   : not referenced by any target here")
