cmake_minimum_required(VERSION 3.25)

project(EpoCanva
  VERSION 0.1.0
  DESCRIPTION "EpoCanva input method engine platform"
  LANGUAGES CXX
)

if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  message(FATAL_ERROR "EpoCanva is meant to be configured as the top-level project, not add_subdirectory'd.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(EPOCANVA_BUILD_TOOLS "Build EpoCanva command line tools" ON)
option(EPOCANVA_BUILD_TESTS "Build EpoCanva infrastructure tests" ON)
# Diagnostic-only, text-free stage tracing to the debug output. OFF for every product/
# package build; ON only in a dedicated diagnosis build.
option(EPOCANVA_PRODUCT_TRACE "Emit text-free TSF stage traces to OutputDebugString" OFF)
if(EPOCANVA_PRODUCT_TRACE)
  add_compile_definitions(EPOCANVA_PRODUCT_TRACE=1)
endif()

add_library(epocanva_strict INTERFACE)
target_compile_features(epocanva_strict INTERFACE cxx_std_17)

if(MSVC)
  target_compile_options(epocanva_strict INTERFACE
    /utf-8
    /permissive-
    /Zc:__cplusplus
    /Zc:preprocessor
    /W4
    /EHsc
    /Gw
  )
  target_compile_definitions(epocanva_strict INTERFACE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
    WIN32_LEAN_AND_MEAN
  )
else()
  target_compile_options(epocanva_strict INTERFACE
    -Wall
    -Wextra
    -Wpedantic
  )
endif()

if(EPOCANVA_BUILD_TOOLS)
  add_subdirectory(tools/toolchain_probe)
endif()

# The English language core includes no librime and no platform header, so it
# builds and unit-tests here, in this project's own CRT, without any of the
# Release/static-CRT librime artefacts in sight. The same two sources are also
# compiled into rime.dll by tools/cmake/epocanva_rime_compiled_in_components.cmake;
# engine/languages/en/CMakeLists.txt is the single definition of that source list.
set(EPOCANVA_EN_BUILD_STANDALONE ON)
add_subdirectory(engine/languages/en)
add_subdirectory(engine/locale)
add_subdirectory(engine/candidates)
add_subdirectory(engine/language_packages)
add_subdirectory(engine/data_store)
add_subdirectory(engine/ranking)

if(EPOCANVA_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests/infrastructure)
  add_subdirectory(tests/english_core)
  add_subdirectory(tests/learning_core)
  add_subdirectory(tests/chinese_core)
  add_subdirectory(tests/french_core)
  add_subdirectory(tests/composition_core)
  add_subdirectory(tests/language_packages)
  add_subdirectory(tests/locale_registry)
  add_subdirectory(tests/candidate_contract)
  add_subdirectory(tests/capability_matrix)
  add_subdirectory(tests/adaptive_ranking)
  if(WIN32)
    add_subdirectory(tests/ci_inputs_lock)
    add_subdirectory(tests/learning_repository)
  endif()
endif()

message(STATUS "EpoCanva configure summary")
message(STATUS "  generator        : ${CMAKE_GENERATOR}")
message(STATUS "  platform         : ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "  compiler ID      : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  C++ standard     : ${CMAKE_CXX_STANDARD} (extensions: ${CMAKE_CXX_EXTENSIONS})")
message(STATUS "  build tools      : ${EPOCANVA_BUILD_TOOLS}")
message(STATUS "  build tests      : ${EPOCANVA_BUILD_TESTS}")
