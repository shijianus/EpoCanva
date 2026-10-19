# EpoCanva P2.1 -- the English language core.
#
# This directory is compiled in TWO contexts and the source list below is the
# single definition of it, so the two cannot drift:
#
#   1. inside project rime, injected by
#      tools/cmake/epocanva_rime_compiled_in_components.cmake, producing the
#      OBJECT library epocanva_en_core_objs whose .obj files end up in rime.dll
#      next to the Translator that calls them (docs/adr/0004);
#   2. inside the top-level EpoCanva project, producing a STATIC library the core
#      unit tests link, which is possible precisely because this code includes no
#      librime header and no platform header (english_lexicon.h).
#
# Nothing is written into this source directory: in context 1 the caller gives
# add_subdirectory an explicit binary directory under the build tree.

set(EPOCANVA_EN_CORE_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/english_lexicon.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/english_case.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/english_token.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/english_space_policy.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/spelling_suggester.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/prefix_completer.cc")
set(EPOCANVA_EN_CORE_HEADERS
  "${CMAKE_CURRENT_SOURCE_DIR}/english_lexicon.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/english_case.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/english_token.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/english_space_policy.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/spelling_suggester.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/prefix_completer.h")

foreach(_epocanva_en_needed ${EPOCANVA_EN_CORE_SOURCES} ${EPOCANVA_EN_CORE_HEADERS})
  if(NOT EXISTS "${_epocanva_en_needed}")
    message(FATAL_ERROR "English core source missing: ${_epocanva_en_needed}")
  endif()
endforeach()

# --- context 1: objects for the engine build -----------------------------------
add_library(epocanva_en_core_objs OBJECT ${EPOCANVA_EN_CORE_SOURCES})
target_include_directories(epocanva_en_core_objs PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(epocanva_en_core_objs PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(epocanva_en_core_objs PRIVATE
    /utf-8 /permissive- /Zc:__cplusplus /W4 /EHsc)
  # The CRT is deliberately NOT set here. Inside project rime it is inherited from
  # upstream's CMAKE_USER_MAKE_RULES_OVERRIDE (third_party/librime/
  # CMakeLists.txt:1-2 -> cmake/cxx_flag_overrides.cmake:5), which is what makes
  # the Release objects /MT; overriding it would be the "force-ignore CRT conflict"
  # the phase brief forbids.
else()
  target_compile_options(epocanva_en_core_objs PRIVATE
    -Wall -Wextra -Wpedantic)
endif()
set_target_properties(epocanva_en_core_objs PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  FOLDER "epocanva")

# Convention the hook reads: each core/component directory names the object
# targets it contributes.
set(epocanva_rime_en_core_object_targets epocanva_en_core_objs PARENT_SCOPE)

# --- context 2: standalone library for the core unit tests ----------------------
if(EPOCANVA_EN_BUILD_STANDALONE)
  add_library(epocanva_en_core STATIC ${EPOCANVA_EN_CORE_SOURCES})
  target_include_directories(epocanva_en_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
  target_compile_features(epocanva_en_core PUBLIC cxx_std_17)
  if(TARGET epocanva_strict)
    target_link_libraries(epocanva_en_core PUBLIC epocanva_strict)
  endif()
  set_target_properties(epocanva_en_core PROPERTIES FOLDER "epocanva")
  message(STATUS "EpoCanva English core: standalone static library epocanva_en_core")
endif()

list(LENGTH EPOCANVA_EN_CORE_SOURCES EPOCANVA_EN_CORE_SOURCE_COUNT)
message(STATUS
  "EpoCanva English core objects: epocanva_en_core_objs (${EPOCANVA_EN_CORE_SOURCE_COUNT} sources)")
