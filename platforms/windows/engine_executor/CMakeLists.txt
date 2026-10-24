# EpoCanva P3.5 -- dedicated single-threaded engine executor.
#
# NOT A STANDALONE PROJECT. This directory is added by platforms/windows/engine_bridge, the
# top-level project that already owns the static-CRT librime bridge. The executor links that
# library; it does not compile P3.2/P3.3 sources again and does not link rime.lib.
#
# WHO OWNS WHAT
#   epocanva_engine_executor  EngineExecutor (queue, lifecycle, capacity, results),
#                             the internal EngineBackend seam, and the real adapter that owns
#                             RimeRuntime + sessions/controllers on the worker thread.
#
# NOT BUILT HERE: no DLL, no TSF registration, no sink, no host text edit, no candidate UI.
if(NOT TARGET epocanva_engine_bridge)
  message(FATAL_ERROR
    "engine_executor must be added by platforms/windows/engine_bridge; it links "
    "epocanva_engine_bridge and does not rebuild the P3.4 sources.")
endif()

add_library(epocanva_engine_executor STATIC
  engine_executor.cc
  engine_executor.h
  engine_executor_internal.h
  engine_backend.h
  real_engine_backend.cc)

target_compile_definitions(epocanva_engine_executor PRIVATE
  _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_compile_options(epocanva_engine_executor PRIVATE
  /W4 /permissive- /EHsc /Zc:__cplusplus /utf-8)

# PUBLIC: EngineExecutor's header includes the P3.4 value/result types, and the test factory
# header is compiled by the executor tests. No engine implementation header is re-exported.
target_include_directories(epocanva_engine_executor PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(epocanva_engine_executor
  PUBLIC epocanva_engine_bridge)
