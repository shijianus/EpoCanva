# EpoCanva P2.1 -- the English component: the Rime-facing half of prefix
# completion, compiled into rime.dll together with the language core it calls.
#
# NOT configured on its own. It is add_subdirectory()'d by
# tools/cmake/epocanva_rime_compiled_in_components.cmake through CMake's
# documented CMAKE_PROJECT_rime_INCLUDE hook, exactly like the P1.4 probe
# directory, and its objects are attached to librime's own `rime` target.
#
# Two object libraries come out of this directory and BOTH have to reach the
# engine, which is why the exported list has two names:
#   epocanva_en_core_objs    -- engine/languages/en (the lexicon and the search)
#   epocanva_english_objs    -- the Translator and the module registration
# Attaching only the second would produce an engine whose Translator references
# functions nothing links, i.e. a link error, not a silently feature-less DLL --
# but attaching only the first would be invisible, so the build entry asserts the
# ATTACHED line names both.

if(NOT EPOCANVA_EN_CORE_DIR)
  message(FATAL_ERROR
    "EPOCANVA_EN_CORE_DIR is not set. The English component calls "
    "engine/languages/en/ and cannot be built without being told where it is.")
endif()
get_filename_component(EPOCANVA_EN_CORE_DIR
  "${EPOCANVA_EN_CORE_DIR}" ABSOLUTE)
if(NOT EXISTS "${EPOCANVA_EN_CORE_DIR}/CMakeLists.txt")
  message(FATAL_ERROR
    "no CMakeLists.txt in EPOCANVA_EN_CORE_DIR: ${EPOCANVA_EN_CORE_DIR}")
endif()
if(NOT EPOCANVA_PROBE_RIME_SOURCE_DIR)
  message(FATAL_ERROR
    "EPOCANVA_PROBE_RIME_SOURCE_DIR is not set; this directory is only meant to "
    "be added by tools/cmake/epocanva_rime_compiled_in_components.cmake.")
endif()

# The core defines epocanva_en_core_objs. EPOCANVA_EN_BUILD_STANDALONE is left
# unset here on purpose, so no second, differently-configured copy of the core is
# created inside the engine build.
add_subdirectory("${EPOCANVA_EN_CORE_DIR}"
                 "${PROJECT_BINARY_DIR}/epocanva-rime-components/en-core")

add_library(epocanva_english_objs OBJECT
  "${CMAKE_CURRENT_SOURCE_DIR}/english_module.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/english_translator.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/english_space_processor.cc")

# Headers this directory needs that the enclosing project would normally provide
# through its own include_directories() calls (third_party/librime/
# CMakeLists.txt:164-166). The hook runs immediately after project(rime), before
# those lines, so every path is stated rather than inherited.
target_include_directories(epocanva_english_objs PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${EPOCANVA_EN_CORE_DIR}"
  "${EPOCANVA_PROBE_RIME_SOURCE_DIR}/src"
  "${EPOCANVA_PROBE_RIME_SOURCE_DIR}/include"
  "${EPOCANVA_PROBE_RIME_BINARY_DIR}/src")
if(EPOCANVA_PROBE_BOOST_INCLUDE_DIR)
  target_include_directories(epocanva_english_objs PRIVATE
    "${EPOCANVA_PROBE_BOOST_INCLUDE_DIR}")
else()
  message(FATAL_ERROR
    "EPOCANVA_PROBE_BOOST_INCLUDE_DIR is empty, but rime/registry.h pulls in "
    "rime/common.h which includes boost/signals2 headers "
    "(third_party/librime/src/rime/common.h:22-26).")
endif()

# C++17 is what the pinned librime builds at (third_party/librime/
# CMakeLists.txt:6). Set per target because the directory-level variable is
# assigned after this hook has run.
target_compile_features(epocanva_english_objs PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(epocanva_english_objs PRIVATE
    /utf-8 /permissive- /Zc:__cplusplus /W4 /EHsc)
else()
  target_compile_options(epocanva_english_objs PRIVATE
    -Wall -Wextra -Wpedantic)
endif()
set_target_properties(epocanva_english_objs PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  FOLDER "epocanva")

# What the hook attaches to the engine target.
set(epocanva_rime_english_object_targets
    epocanva_english_objs epocanva_en_core_objs PARENT_SCOPE)

message(STATUS "EpoCanva English component: epocanva_english_objs "
               "(english_module.cc, english_translator.cc, english_space_processor.cc) "
               "+ epocanva_en_core_objs")
message(STATUS "  component dir   : ${CMAKE_CURRENT_SOURCE_DIR}")
message(STATUS "  language core   : ${EPOCANVA_EN_CORE_DIR}")
message(STATUS "  rime source dir : ${EPOCANVA_PROBE_RIME_SOURCE_DIR}")
message(STATUS "  generated incl  : ${EPOCANVA_PROBE_RIME_BINARY_DIR}/src")
