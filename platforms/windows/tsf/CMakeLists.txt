# EpoCanva P3.1 -- the unregistered Windows TSF/COM lifecycle skeleton.
#
# STANDALONE ON PURPOSE (P3.1 brief, section 4). This is its own top-level project,
# configured with the VS 2022 generator into its own build directory
# (build/windows-tsf/), because:
#   - the DLL and its tests need only the Windows SDK, never librime, never the engine,
#     never third_party -- rebuilding those here would couple this round's lifetime to
#     the engine's;
#   - the root EpoCanva project is a Debug / dynamic-CRT build, and mixing that with a
#     DLL meant to be loaded into arbitrary hosts would drag two CRTs into one process.
#
# WHAT IS BUILT HERE
#   epocanva_tsf          the text service DLL (no registration exports; see
#                         epocanva_tsf.def)
#   tsf_lifecycle         the verification program from tests/windows/tsf_lifecycle,
#                         built from the SAME identifiers.cc as the DLL so the CLSID has
#                         exactly one definition in the repository
#
# CRT: static (/MT via CMAKE_MSVC_RUNTIME_LIBRARY below). A TSF DLL is loaded into
# host processes we do not choose; depending on a redistributable CRT would add an
# install-time promise this project cannot keep yet. The test program uses the same
# setting; the two binaries never share CRT state (the test reaches the DLL only
# through LoadLibrary + GetProcAddress + COM interfaces).
#
# REGISTRATION: none. Nothing here writes the registry, and no target produces an
# installer. The DLL is exercised by absolute path only (docs/windows/tsf-bootstrap.md).
cmake_minimum_required(VERSION 3.25)
project(epocanva_windows_tsf VERSION 0.1.0 LANGUAGES CXX)

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
    "The Windows TSF front end is built with the MSVC toolchain only.")
endif()

if(NOT CMAKE_GENERATOR_PLATFORM STREQUAL "x64")
  message(WARNING
    "CMAKE_GENERATOR_PLATFORM is '${CMAKE_GENERATOR_PLATFORM}', not x64. The TSF DLL "
    "is only built for x64.")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

set(EpoCanva_Tsf_Static_Crt TRUE)  # recorded for the configure summary below

add_library(epocanva_tsf SHARED
  dll_main.cc
  identifiers.cc
  module_state.cc
  class_factory.cc
  text_service.cc
  epocanva_tsf.def)
target_compile_definitions(epocanva_tsf PRIVATE
  _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_compile_options(epocanva_tsf PRIVATE
  /W4 /permissive- /EHsc /Zc:__cplusplus /utf-8)

# The .def file is the only export mechanism: no __declspec(dllexport) exists anywhere
# in the sources, so the export table and the .def file cannot drift apart.

# --- the verification program -------------------------------------------------------
# Included from the DLL project so one configure builds both, and so the test target can
# depend on (and reference the built path of) the DLL. The subdirectory refuses to be
# configured on its own -- see its guard.
add_subdirectory(
  "${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/windows/tsf_lifecycle"
  "${CMAKE_CURRENT_BINARY_DIR}/tsf_lifecycle")

enable_testing()

message(STATUS "epocanva_windows_tsf summary")
message(STATUS "  generator            : ${CMAKE_GENERATOR}")
message(STATUS "  platform             : ${CMAKE_GENERATOR_PLATFORM}")
message(STATUS "  configuration types  : ${CMAKE_CONFIGURATION_TYPES}")
message(STATUS "  MSVC runtime library : ${CMAKE_MSVC_RUNTIME_LIBRARY} (static CRT)")
message(STATUS "  registration         : none -- no registry write, no regsvr32, "
               "no profile/category registration")
message(STATUS "  engine/third_party   : not referenced by any target here")
