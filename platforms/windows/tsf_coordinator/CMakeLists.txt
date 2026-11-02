# EpoCanva P3.8 -- the context-binding coordinator.
#
# NOT A STANDALONE PROJECT. This directory is added by platforms/windows/engine_bridge, the
# top-level project that already owns the static-CRT engine bridge and executor. The
# coordinator links those existing targets; it does not recompile the P3.2/P3.3/P3.4
# sources and does not re-link rime.
#
# WHO OWNS WHAT
#   epocanva_tsf_edit_layer  the P3.7 edit layer (EditPlan/EditResult/CompositionWriter/
#                            CompositionEditSession), compiled once for this build tree
#                            from its one set of sources in ../tsf_edit. The standalone
#                            P3.7 project builds the same sources under its own tree; the
#                            strict P3.2 converter symbols are NOT recompiled here -- they
#                            come from epocanva_engine_text, which this tree already has.
#   epocanva_tsf_coordinator the coordinator: one bound context, one in-flight work item,
#                            value commands to the executor, edit plans to the P3.7 layer.
#
# NOT BUILT HERE: no DLL, no input-method registration, no key sink, no candidate window,
# no connection to the real TSF service activation path, no write into any application.
if(NOT TARGET epocanva_engine_executor)
  message(FATAL_ERROR
    "tsf_coordinator must be added by platforms/windows/engine_bridge; it links "
    "epocanva_engine_executor and the P3.7 edit layer, and does not rebuild them.")
endif()

set(_coordinator_tsf_edit_dir "${CMAKE_CURRENT_SOURCE_DIR}/../tsf_edit")

add_library(epocanva_tsf_edit_layer STATIC
  "${_coordinator_tsf_edit_dir}/edit_plan.cc"
  "${_coordinator_tsf_edit_dir}/edit_plan.h"
  "${_coordinator_tsf_edit_dir}/edit_result.cc"
  "${_coordinator_tsf_edit_dir}/edit_result.h"
  "${_coordinator_tsf_edit_dir}/composition_writer.cc"
  "${_coordinator_tsf_edit_dir}/composition_writer.h"
  "${_coordinator_tsf_edit_dir}/edit_session.cc"
  "${_coordinator_tsf_edit_dir}/edit_session.h")
target_compile_definitions(epocanva_tsf_edit_layer PRIVATE
  _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_compile_options(epocanva_tsf_edit_layer PRIVATE
  /W4 /permissive- /EHsc /Zc:__cplusplus /utf-8)
target_include_directories(epocanva_tsf_edit_layer PUBLIC
  "${_coordinator_tsf_edit_dir}")
# text_encoding.h comes from epocanva_engine_text's PUBLIC include dir; its symbols are
# likewise taken from that library, not compiled a second time here. uuid supplies the
# msctf IID definitions and ole32 the COM entry points, exactly as in the P3.7 project.
target_link_libraries(epocanva_tsf_edit_layer
  PUBLIC epocanva_engine_text ole32 uuid)

add_library(epocanva_tsf_coordinator STATIC
  coordinator.h
  coordinator.cc
  backend_integration.cc
  engine_to_plan.h
  engine_to_plan.cc
  observed_binding.h
  observed_binding.cc)
target_compile_definitions(epocanva_tsf_coordinator PRIVATE
  _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_compile_options(epocanva_tsf_coordinator PRIVATE
  /W4 /permissive- /EHsc /Zc:__cplusplus /utf-8)
target_include_directories(epocanva_tsf_coordinator PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(epocanva_tsf_coordinator
  PUBLIC epocanva_tsf_edit_layer epocanva_engine_executor epocanva_tsf_context)
