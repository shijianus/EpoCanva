# EpoCanva P3.4 -- the controlled librime runtime/session bridge.
#
# STANDALONE ON PURPOSE, following P3.1-P3.3: its own top-level project and build directory
# (build/windows-engine-bridge/test), static CRT (/MT) so a later link into epocanva_tsf.dll
# does not mix runtimes, and no add_subdirectory() from the root project.
#
# WHAT IS BUILT HERE
#   epocanva_engine_text        P3.2's strict converter and boundary map, compiled from
#                               ../text/text_encoding.cc and ../text/text_boundary_map.cc -- the
#                               SAME sources, not a second codec.
#   epocanva_engine_bridge      rime_runtime.{h,cc}, rime_session.{h,cc},
#                               engine_update.{h,cc}. It LOADS rime.dll by absolute path with
#                               LoadLibraryExW + GetProcAddress; it does not link rime.lib and
#                               therefore cannot accidentally bind to another build.
#   engine_bridge_tests         the verification program from tests/windows/engine_bridge.
#
# WHAT IS NOT BUILT HERE: no input method DLL, no TSF registration, no key sink, no host text
# editing, no candidate UI. Nothing in this project registers anything with the OS.
#
# The RimeApi header comes from the pinned checkout (third_party/librime/src/rime_api.h), the
# same public header the installed DLL was built from. The DLL itself is named by
# EPOCANVA_RIME_DLL and must already exist; this project never builds, downloads or patches it.
cmake_minimum_required(VERSION 3.25)
project(epocanva_windows_engine_bridge VERSION 0.1.0 LANGUAGES CXX)

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
    "The engine bridge is a Windows/MSVC target: it loads a PE DLL and calls Win32 APIs.")
endif()

if(NOT CMAKE_GENERATOR_PLATFORM STREQUAL "x64")
  message(WARNING
    "CMAKE_GENERATOR_PLATFORM is '${CMAKE_GENERATOR_PLATFORM}', not x64. The installed "
    "rime.dll and the rest of the Windows layer are x64.")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

set(_bridge_definitions
  _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
set(_bridge_options /W4 /permissive- /EHsc /Zc:__cplusplus /utf-8)

# Public librime API header, pinned in the repository.
set(EPOCANVA_RIME_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../third_party/librime/src" CACHE PATH
  "Directory holding the pinned rime_api.h")
find_file(_bridge_rime_api rime_api.h PATHS "${EPOCANVA_RIME_INCLUDE_DIR}"
          NO_DEFAULT_PATH REQUIRED)
message(STATUS "  rime_api.h           : ${_bridge_rime_api}")

set(EPOCANVA_ENGLISH_COMPONENT_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../engine/rime_components/english" CACHE PATH
  "Directory holding english_diagnostics.h (module name and custom API)")
find_file(_bridge_english_header english_diagnostics.h
          PATHS "${EPOCANVA_ENGLISH_COMPONENT_DIR}" NO_DEFAULT_PATH REQUIRED)
message(STATUS "  english_diagnostics  : ${_bridge_english_header}")

# --- P3.2's converter and boundary table, from their one set of sources -----------------
add_library(epocanva_engine_text STATIC
  "${CMAKE_CURRENT_SOURCE_DIR}/../text/text_encoding.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/../text/text_encoding.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/../text/text_boundary_map.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/../text/text_boundary_map.h")
target_compile_definitions(epocanva_engine_text PRIVATE ${_bridge_definitions})
target_compile_options(epocanva_engine_text PRIVATE ${_bridge_options})
target_include_directories(epocanva_engine_text PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/../text")

# --- runtime, session and result copies ------------------------------------------------
add_library(epocanva_engine_bridge STATIC
  rime_runtime.cc
  rime_runtime.h
  rime_runtime_internal.h
  rime_session.cc
  rime_session.h
  engine_update.cc
  engine_update.h)
target_compile_definitions(epocanva_engine_bridge PRIVATE ${_bridge_definitions})
target_compile_options(epocanva_engine_bridge PRIVATE ${_bridge_options})
target_include_directories(epocanva_engine_bridge
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
  PRIVATE
    "${EPOCANVA_RIME_INCLUDE_DIR}"
    "${EPOCANVA_ENGLISH_COMPONENT_DIR}")
# PUBLIC: engine_update.h includes text_boundary_map.h, so every consumer compiles against
# the identical P3.2 declarations. No rime.lib is linked: the API table arrives through
# GetProcAddress at run time.
target_link_libraries(epocanva_engine_bridge PUBLIC epocanva_engine_text)

# --- test inputs: existing artefacts, never rebuilt here -------------------------------
set(EPOCANVA_RIME_DLL
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../build/librime-english/rime/install/lib/rime.dll"
    CACHE FILEPATH "Absolute path of the ENGLISH rime.dll the tests must load")
if(NOT EXISTS "${EPOCANVA_RIME_DLL}")
  message(FATAL_ERROR
    "EPOCANVA_RIME_DLL does not exist: ${EPOCANVA_RIME_DLL}\n"
    "This project tests an already-built English engine; it does not build one.")
endif()
message(STATUS "  English rime.dll     : ${EPOCANVA_RIME_DLL}")

set(EPOCANVA_BASELINE_RIME_DLL
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../build/librime-baseline/rime/install/lib/rime.dll"
    CACHE FILEPATH
  "Optional unmodified rime.dll used to prove the missing-module failure; empty/absent skips")
if(EXISTS "${EPOCANVA_BASELINE_RIME_DLL}")
  set(_bridge_baseline_dll "${EPOCANVA_BASELINE_RIME_DLL}")
  message(STATUS "  baseline rime.dll    : ${_bridge_baseline_dll}")
else()
  set(_bridge_baseline_dll "")
  message(WARNING
    "EPOCANVA_BASELINE_RIME_DLL does not exist; the module-missing case will report itself "
    "as unverified rather than passing.")
endif()

set(EPOCANVA_ENGLISH_FIXTURE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/fixtures/english_completion" CACHE PATH
  "Hand-written English completion fixture (never written to)")
set(EPOCANVA_SPELLING_FIXTURE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/fixtures/english_spelling" CACHE PATH
  "Hand-written spelling fixture, used only for a candidate with a non-empty comment")
foreach(_fixture_file
        "${EPOCANVA_ENGLISH_FIXTURE_DIR}/default.yaml"
        "${EPOCANVA_ENGLISH_FIXTURE_DIR}/epocanva_test_english.schema.yaml"
        "${EPOCANVA_ENGLISH_FIXTURE_DIR}/lexicon.tsv"
        "${EPOCANVA_SPELLING_FIXTURE_DIR}/epocanva_spell_english.schema.yaml"
        "${EPOCANVA_SPELLING_FIXTURE_DIR}/lexicon_spell.tsv")
  if(NOT EXISTS "${_fixture_file}")
    message(FATAL_ERROR "required fixture file is missing: ${_fixture_file}")
  endif()
endforeach()

add_subdirectory(
  "${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/windows/engine_bridge"
  "${CMAKE_CURRENT_BINARY_DIR}/engine_bridge")

enable_testing()

message(STATUS "epocanva_windows_engine_bridge summary")
message(STATUS "  generator            : ${CMAKE_GENERATOR}")
message(STATUS "  platform             : ${CMAKE_GENERATOR_PLATFORM}")
message(STATUS "  configuration types  : ${CMAKE_CONFIGURATION_TYPES}")
message(STATUS "  MSVC runtime library : ${CMAKE_MSVC_RUNTIME_LIBRARY} (static CRT)")
message(STATUS "  rime linkage         : none -- LoadLibraryExW(abs path) + GetProcAddress")
message(STATUS "  registration         : none -- no DLL target, no registry write, no sink")
message(STATUS "  deployment           : explicit one-shot preparation only; never per key/session")
