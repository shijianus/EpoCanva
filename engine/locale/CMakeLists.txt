cmake_minimum_required(VERSION 3.25)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(EpoCanvaLocale LANGUAGES CXX)
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

# Identity and admission only: no platform header, no librime header, no I/O.
add_library(epocanva_locale STATIC
  language_tag.cc
  language_tag.h
  locale_profile.cc
  locale_profile.h)
target_include_directories(epocanva_locale PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(epocanva_locale PRIVATE epocanva_strict)
set_target_properties(epocanva_locale PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
