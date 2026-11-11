cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(EpoCanvaCandidates LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
endif()

# No platform header, no C ABI header, no I/O. This is the internal candidate
# contract shared by language sources and the backend adapter.
add_library(epocanva_candidates STATIC
  candidate_contract.cc
  candidate_contract.h)
target_include_directories(epocanva_candidates PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
if(TARGET epocanva_strict)
  target_link_libraries(epocanva_candidates PRIVATE epocanva_strict)
elseif(MSVC)
  target_compile_options(epocanva_candidates PRIVATE /utf-8 /permissive- /W4 /EHsc)
endif()
set_target_properties(epocanva_candidates PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
