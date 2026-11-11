# Reusable API target. A product build that already owns epocanva_engine_bridge
# links that exact target: never compile a second runtime into the same process.
# The independent tests build the SAME legacy sources in one private C++ library.
cmake_minimum_required(VERSION 3.25)
if(NOT WIN32 OR NOT MSVC)
  message(FATAL_ERROR "ABI v1 has a real Windows/MSVC Rime adapter only; no stub backend exists")
endif()
get_filename_component(_engine_repo "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(_engine_build_root "${_engine_repo}/build")
cmake_path(IS_PREFIX _engine_build_root "${CMAKE_BINARY_DIR}" NORMALIZE _engine_in_build)
if(NOT _engine_in_build OR CMAKE_BINARY_DIR STREQUAL _engine_build_root)
  message(FATAL_ERROR "Build artifacts must stay inside this repository's build/<preset>/")
endif()

if(TARGET epocanva_engine_bridge)
  set(_engine_backend epocanva_engine_bridge)
else()
  set(_engine_bridge "${_engine_repo}/platforms/windows/engine_bridge")
  set(_engine_text "${_engine_repo}/platforms/windows/text")
  add_library(epocanva_engine_runtime_cpp STATIC
    "${_engine_bridge}/rime_runtime.cc"
    "${_engine_bridge}/rime_session.cc"
    "${_engine_bridge}/engine_update.cc"
    "${_engine_text}/text_encoding.cc"
    "${_engine_text}/text_boundary_map.cc")
  target_include_directories(epocanva_engine_runtime_cpp PUBLIC
    "${_engine_bridge}" "${_engine_text}"
    PRIVATE "${_engine_repo}/third_party/librime/src"
            "${_engine_repo}/engine/rime_components/english")
  target_compile_definitions(epocanva_engine_runtime_cpp PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
  target_compile_options(epocanva_engine_runtime_cpp PRIVATE
    /W4 /permissive- /EHsc /Zc:__cplusplus /utf-8)
  set_target_properties(epocanva_engine_runtime_cpp PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  set(_engine_backend epocanva_engine_runtime_cpp)
endif()

add_library(epocanva_engine_api STATIC
  engine_api.cc "${CMAKE_CURRENT_LIST_DIR}/../adapter/rime_adapter.cc")
target_include_directories(epocanva_engine_api PUBLIC
  "${CMAKE_CURRENT_LIST_DIR}/../include"
  PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../adapter")
target_compile_definitions(epocanva_engine_api PRIVATE
  NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_compile_options(epocanva_engine_api PRIVATE
  /W4 /permissive- /EHsc /Zc:__cplusplus /utf-8)
set_target_properties(epocanva_engine_api PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_link_libraries(epocanva_engine_api PRIVATE ${_engine_backend})
# No rime.lib, download, install, registration, product target or runtime fork.
