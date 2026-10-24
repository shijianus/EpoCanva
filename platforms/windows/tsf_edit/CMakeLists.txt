# EpoCanva P3.7 -- controlled TSF test host and composition edit sessions.
#
# STANDALONE ON PURPOSE, following platforms/windows/text and platforms/windows/tsf: this
# round needs the Windows SDK and the real msctf.dll, nothing from librime, the engine or
# third_party. Its static CRT (/MT) matches the CRT the TSF DLL will carry, so the edit layer
# built here can later be linked into epocanva_tsf without mixing two CRTs in one process.
#
# WHAT IS BUILT HERE
#   epocanva_tsf_edit   the immutable EditPlan, EditResult, CompositionWriter and
#                       CompositionEditSession (the layer that will later be linked into the
#                       TSF DLL), plus the P3.2 strict text conversion it reuses
#   tsf_edit_tests      tests/windows/tsf_edit: the controlled ITextStoreACP host and the
#                       composition-layer cases, all against the real TSF manager
#
# WHAT IS NOT BUILT HERE: no DLL, no registration export, no keyboard sink, no candidate UI,
# no AdviseKeyEventSink, and no call into an external application. Nothing in this directory
# writes the registry or installs anything.
cmake_minimum_required(VERSION 3.25)
project(epocanva_windows_tsf_edit VERSION 0.1.0 LANGUAGES CXX)

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
    "The Windows TSF edit layer is built with the MSVC toolchain only.")
endif()

if(NOT CMAKE_GENERATOR_PLATFORM STREQUAL "x64")
  message(WARNING
    "CMAKE_GENERATOR_PLATFORM is '${CMAKE_GENERATOR_PLATFORM}', not x64. The TSF document "
    "host and the edit layer are only built for x64.")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

# The P3.2 text project deliberately refuses add_subdirectory, so its strict converter is
# compiled from the same source file here instead of being reimplemented. This is the only
# duplication: the API and implementation stay in platforms/windows/text.
add_library(epocanva_tsf_edit STATIC
  edit_plan.cc
  edit_plan.h
  edit_result.cc
  edit_result.h
  composition_writer.cc
  composition_writer.h
  edit_session.cc
  edit_session.h
  "${CMAKE_CURRENT_SOURCE_DIR}/../text/text_encoding.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/../text/text_encoding.h")
target_compile_definitions(epocanva_tsf_edit PRIVATE
  _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_compile_options(epocanva_tsf_edit PRIVATE
  /W4 /permissive- /EHsc /Zc:__cplusplus /utf-8)
target_include_directories(epocanva_tsf_edit PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(epocanva_tsf_edit PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../text")
# uuid supplies the IID_ITfRangeACP / IID_ITfContextComposition / IID_ITfInsertAtSelection
# definitions; ole32 supplies the COM entry points. No msctf.lib is linked.
target_link_libraries(epocanva_tsf_edit PUBLIC ole32 uuid)

set(EPOCANVA_TSF_EDIT_TOP_LEVEL ON)

add_subdirectory(
  "${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/windows/tsf_edit"
  "${CMAKE_CURRENT_BINARY_DIR}/tsf_edit_tests")

enable_testing()

message(STATUS "epocanva_windows_tsf_edit summary")
message(STATUS "  generator            : ${CMAKE_GENERATOR}")
message(STATUS "  platform             : ${CMAKE_GENERATOR_PLATFORM}")
message(STATUS "  configuration types  : ${CMAKE_CONFIGURATION_TYPES}")
message(STATUS "  MSVC runtime library : ${CMAKE_MSVC_RUNTIME_LIBRARY} (static CRT)")
message(STATUS "  real TSF dependency  : msctf.dll through COM (CLSID_TF_ThreadMgr)")
message(STATUS "  registration         : none -- no registry write, no input-language entry")
message(STATUS "  engine/third_party   : not referenced by any target here")
