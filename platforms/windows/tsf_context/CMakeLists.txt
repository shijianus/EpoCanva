# EpoCanva P3.9 -- real input-permission resolution from TSF context metadata.
#
# NOT A STANDALONE PROJECT. This directory is added by platforms/windows/engine_bridge,
# the top-level project that owns the static-CRT engine/executor/edit build. The module
# links nothing of the engine; its only platform dependencies are ole32 (VariantClear,
# CoTaskMemFree) and uuid (the msctf/inputscope GUID and IID definitions).
#
# WHAT IS BUILT HERE
#   epocanva_tsf_context   input_permission.{h,cc} -- the pure, COM-free classification
#                          of observed context facts into Allowed/Denied/Unknown with
#                          findings; permission_query.{h,cc} -- the PermissionSource seam
#                          and the real implementation that reads ITfContext::GetStatus,
#                          the KEYBOARD_DISABLED/EMPTYCONTEXT context compartments and
#                          the GUID_PROP_INPUTSCOPE application property through a real
#                          read-only edit session.
#
# NOT BUILT HERE: no DLL, no registration, no key sink, no candidate UI, no focus
# tracking, no engine calls, no document text reads.
if(NOT TARGET epocanva_engine_executor)
  message(FATAL_ERROR
    "tsf_context must be added by platforms/windows/engine_bridge; it shares that "
    "project's static-CRT setting and is consumed by the coordinator target.")
endif()

add_library(epocanva_tsf_context STATIC
  input_permission.h
  input_permission.cc
  permission_query.h
  permission_query.cc
  inputscope_guids.cc)
target_compile_definitions(epocanva_tsf_context PRIVATE
  _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_compile_options(epocanva_tsf_context PRIVATE
  /W4 /permissive- /EHsc /Zc:__cplusplus /utf-8)
target_include_directories(epocanva_tsf_context PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(epocanva_tsf_context PUBLIC ole32 uuid)
