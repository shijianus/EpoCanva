cmake_minimum_required(VERSION 3.25)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(EpoCanvaFusion LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
endif()

if(NOT TARGET epocanva_strict)
  add_library(epocanva_strict INTERFACE)
  target_compile_features(epocanva_strict INTERFACE cxx_std_17)
  if(MSVC)
    target_compile_options(epocanva_strict INTERFACE /utf-8 /permissive- /Zc:__cplusplus /W4 /EHsc)
  else()
    target_compile_options(epocanva_strict INTERFACE -Wall -Wextra -Wpedantic)
  endif()
endif()

if(NOT TARGET epocanva_candidates)
  add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../candidates" candidates-core)
endif()

# Pure fusion logic: no platform header, no I/O, no network, no persistence.
# The policy parameters are recorded evidence compiled into the module; the
# machine-readable mirror lives in data/eval/candidate-v1/fusion-policy.json
# and a test pins the equality.
add_library(epocanva_fusion STATIC
  fusion_policy.cc
  fusion_policy.h
  fusion_merger.cc
  fusion_merger.h)
target_include_directories(epocanva_fusion PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(epocanva_fusion PUBLIC epocanva_candidates PRIVATE epocanva_strict)
set_target_properties(epocanva_fusion PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
if(MSVC)
  target_compile_options(epocanva_fusion PRIVATE /utf-8 /permissive- /W4 /EHsc)
endif()
