cmake_minimum_required(VERSION 3.25)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(EpoCanvaRanking LANGUAGES CXX)
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

# Pure ranking logic: no platform header, no librime header, no persistence and
# no network. The learning model is in-memory only in this revision.
add_library(epocanva_adaptive_ranking STATIC
  adaptive_ranker.cc
  adaptive_ranker.h
  learning_events.cc
  learning_events.h)
target_include_directories(epocanva_adaptive_ranking PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(epocanva_adaptive_ranking PRIVATE epocanva_strict)
set_target_properties(epocanva_adaptive_ranking PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
