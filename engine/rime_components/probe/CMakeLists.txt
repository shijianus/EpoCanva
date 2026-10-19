# EpoCanva P1.4 -- object library holding EpoCanva's own librime components.
#
# This file is NOT configured on its own. It is add_subdirectory()'d from
# tools/cmake/epocanva_rime_compiled_in_components.cmake, which CMake includes
# through the documented CMAKE_PROJECT_rime_INCLUDE hook, and the resulting
# objects are attached to librime's own `rime` target. The consequence, which is
# the decision recorded in docs/adr/0004-compiled-in-rime-components.md: these
# .obj files end up INSIDE rime.dll, so the component and the engine share one PE
# module, one CRT and one heap.
#
# No artefact is written here: everything lands in the caller's build tree
# (${CMAKE_BINARY_DIR}/epocanva-rime-components), never in this source directory.
#
# Upstream's own plugin boilerplate is the model for this file
# (third_party/librime/sample/CMakeLists.txt:37-47, plugins/CMakeLists.txt:13-21),
# except that upstream reaches it only for directories that physically live under
# third_party/librime/plugins/ -- which is why a project-side adaptation exists.

# The hook runs immediately after upstream's project(rime) call, i.e. BEFORE
# third_party/librime/CMakeLists.txt:6 sets CMAKE_CXX_STANDARD and before :164-166
# set the include directories. So nothing is inherited by accident: the standard
# and every include directory are stated explicitly below, and each one is
# verified rather than assumed.

foreach(_required EPOCANVA_PROBE_RIME_SOURCE_DIR EPOCANVA_PROBE_RIME_BINARY_DIR)
  if(NOT ${_required})
    message(FATAL_ERROR
      "${_required} is not set. This directory is only meant to be added by "
      "tools/cmake/epocanva_rime_compiled_in_components.cmake.")
  endif()
endforeach()

foreach(_needed
      "${EPOCANVA_PROBE_RIME_SOURCE_DIR}/src/rime/registry.h"
      "${EPOCANVA_PROBE_RIME_SOURCE_DIR}/include/X11/keysym.h")
  if(NOT EXISTS "${_needed}")
    message(FATAL_ERROR
      "expected librime source tree layout to provide ${_needed}, it is absent. "
      "The pinned checkout moved; re-verify the include paths instead of guessing.")
  endif()
endforeach()

add_library(epocanva_rime_probe_objs OBJECT
  probe_module.cc
  probe_processor.cc)

# C++17 is what the pinned librime builds at (third_party/librime/CMakeLists.txt:6)
# and rime/common.h needs it (std::filesystem, nested namespace macros). Set per
# target because the directory-level variable is only assigned after this hook
# runs. CMAKE_CXX_EXTENSIONS has no effect on MSVC; the value is inherited from
# the caller and extensions were never a knob MSVC exposes.
target_compile_features(epocanva_rime_probe_objs PRIVATE cxx_std_17)

# rime/registry.h -> rime/common.h -> <rime/build_config.h> (generated) and
# <boost/signals2/...>; rime/key_table.h -> <X11/keysym.h>, which librime vendors
# in its own include/ directory.
target_include_directories(epocanva_rime_probe_objs PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${EPOCANVA_PROBE_RIME_SOURCE_DIR}/src"
  "${EPOCANVA_PROBE_RIME_SOURCE_DIR}/include"
  "${EPOCANVA_PROBE_RIME_BINARY_DIR}/src")
if(EPOCANVA_PROBE_BOOST_INCLUDE_DIR)
  target_include_directories(epocanva_rime_probe_objs PRIVATE
    "${EPOCANVA_PROBE_BOOST_INCLUDE_DIR}")
else()
  message(FATAL_ERROR
    "EPOCANVA_PROBE_BOOST_INCLUDE_DIR is empty, but rime/common.h includes "
    "boost/signals2 headers (third_party/librime/src/rime/common.h:22-26).")
endif()

target_compile_options(epocanva_rime_probe_objs PRIVATE
  /utf-8 /permissive- /Zc:__cplusplus /W4 /EHsc)

# Matches plugins/CMakeLists.txt:14-18 for the shared-library build; a no-op on
# MSVC, kept so the same file stays correct if this ever configures on POSIX.
set_target_properties(epocanva_rime_probe_objs PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  FOLDER "epocanva")

# Contract with the hook (P2.1 generalisation): a component directory names the
# object targets it contributes, and the hook refuses a component that declares
# none rather than compiling it and leaving it out of the engine.
set(epocanva_rime_probe_object_targets epocanva_rime_probe_objs PARENT_SCOPE)

message(STATUS "EpoCanva component: epocanva_rime_probe_objs (probe_module.cc, probe_processor.cc)")
message(STATUS "  sources            : ${CMAKE_CURRENT_SOURCE_DIR}")
message(STATUS "  rime source dir    : ${EPOCANVA_PROBE_RIME_SOURCE_DIR}")
message(STATUS "  generated headers  : ${EPOCANVA_PROBE_RIME_BINARY_DIR}/src")
message(STATUS "  Boost include dir  : ${EPOCANVA_PROBE_BOOST_INCLUDE_DIR}")
