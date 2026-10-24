# EpoCanva P3.7 -- controlled TSF test host and composition edit sessions.
#
# STANDALONE ON PURPOSE, following platforms/windows/text and platforms/windows/tsf: this
# round needs the Windows SDK and the real msctf.dll, nothing from librime, the engine or
# third_party. Its static CRT (/MT) matches the CRT the TSF DLL will carry, so the edit layer
# built here can later be linked into epocanva_tsf without mixing two CRTs in one process.
#
# WHAT IS BUILT HERE (stage 1): the verification program tests/windows/tsf_edit, which owns
# the controlled ITextStoreACP host. Stage 2 adds the edit layer library below.
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
