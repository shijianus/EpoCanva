# EpoCanva -- project-side CMake adaptation that puts EpoCanva's own rime
# components INSIDE the engine library.
#
# Introduced in P1.4 for the probe component; generalised in P2.1 to take a list
# of components, because the English completion component needs the language core
# compiled in next to it. The mechanism is unchanged.
#
# How it is used (see tools/build_librime_baseline.ps1, -Mode component-probe and
# -Mode english-completion):
#
#     cmake -S third_party/librime -B build/<mode>/rime \
#           -DCMAKE_PROJECT_rime_INCLUDE=<repo>/tools/cmake/epocanva_rime_compiled_in_components.cmake \
#           -DEPOCANVA_RIME_COMPONENT_ROOT=<repo>/engine/rime_components \
#           -DEPOCANVA_RIME_COMPONENT_NAMES="english" \
#           -DEPOCANVA_RIME_HOOK_LABEL=P2.1 \
#           -DEPOCANVA_EN_CORE_DIR=<repo>/engine/languages/en \
#           -DEPOCANVA_BOOST_INCLUDE_DIR=<boost tree>
#
# CMake includes this file as the last step of the project(rime) call
# (third_party/librime/CMakeLists.txt:5). CMake's own documentation for the
# variable says it "is intended for injecting custom code into a project's build
# without modifying their source" -- which is precisely the constraint this file
# exists to satisfy: AGENTS.md section 7 forbids touching the upstream tree, and
# upstream's plugin mechanism only accepts directories that physically live under
# third_party/librime/plugins/ (plugins/CMakeLists.txt:24-47 strips every
# directory prefix off ENV{RIME_PLUGINS} at :29-30 and otherwise globs plugins/*;
# sample/CMakeLists.txt:36 states the official instruction is "link or copy the
# directory to plugins/"). There is therefore no upstream entry point that takes
# an out-of-tree source path, and no upstream file is edited here.
#
# At include time we are in the top-level directory scope of project rime, before
# upstream's add_subdirectory(plugins) (:264) and add_subdirectory(src) (:276).
# The `rime` target does not exist yet, so the objects cannot be attached here;
# they are attached by a deferred call, which CMake documents as running "as if
# written at the end of the current directory's CMakeLists.txt file"
# (cmake_language(DEFER), CMake >= 3.19; the pinned toolchain here is 3.31.6).
#
# What this does NOT do: it does not add a module name to kDefaultModules. That
# would need RIME_SETUP_EXTRA_MODULES, which upstream derives only from plugins
# found under plugins/ (CMakeLists.txt:264-271, src/CMakeLists.txt:174-177).
# The front end names the module in RimeTraits.modules instead -- a public C API
# field that RimeInitialize feeds straight into LoadModules
# (src/rime_api_impl.h:51-54). See docs/adr/0004-compiled-in-rime-components.md.

if(NOT PROJECT_NAME STREQUAL "rime")
  message(FATAL_ERROR
    "epocanva_rime_compiled_in_components.cmake was included by project "
    "'${PROJECT_NAME}'. It is wired to CMAKE_PROJECT_rime_INCLUDE and only knows "
    "how to attach to the rime build.")
endif()

if(NOT EPOCANVA_RIME_COMPONENT_NAMES)
  message(FATAL_ERROR
    "CMAKE_PROJECT_rime_INCLUDE points at the EpoCanva component hook but "
    "EPOCANVA_RIME_COMPONENT_NAMES is empty. A build configured this way would "
    "produce an engine that looks like a component build and is not one.")
endif()

if(NOT EPOCANVA_RIME_HOOK_LABEL)
  message(FATAL_ERROR
    "EPOCANVA_RIME_HOOK_LABEL is not set. The build entry point asserts exactly "
    "one 'EpoCanva <label> ATTACHED:' line in the configure output, and it cannot "
    "do that without knowing which label this mode uses.")
endif()

if(NOT EPOCANVA_RIME_COMPONENT_ROOT)
  message(FATAL_ERROR "EPOCANVA_RIME_COMPONENT_ROOT is not set.")
endif()
get_filename_component(EPOCANVA_RIME_COMPONENT_ROOT
  "${EPOCANVA_RIME_COMPONENT_ROOT}" ABSOLUTE)
if(NOT IS_DIRECTORY "${EPOCANVA_RIME_COMPONENT_ROOT}")
  message(FATAL_ERROR
    "EPOCANVA_RIME_COMPONENT_ROOT is not a directory: ${EPOCANVA_RIME_COMPONENT_ROOT}")
endif()

foreach(_flag BUILD_MERGED_PLUGINS BUILD_SHARED_LIBS)
  if(NOT ${_flag})
    message(FATAL_ERROR
      "EpoCanva components require ${_flag}=ON (one PE module holding engine and "
      "component). It is '${${_flag}}'.")
  endif()
endforeach()
if(ENABLE_EXTERNAL_PLUGINS)
  message(FATAL_ERROR
    "ENABLE_EXTERNAL_PLUGINS must stay OFF for EpoCanva: the module it compiles in "
    "(third_party/librime/plugins/plugins_module.cc:35-70) loads every *.dll it "
    "finds in a rime-plugins directory, which EpoCanva's phase brief forbids.")
endif()

# Everything the components need to compile against, resolved from the project we
# have just entered. Verified as existing, not assumed.
set(EPOCANVA_PROBE_RIME_SOURCE_DIR "${PROJECT_SOURCE_DIR}")
set(EPOCANVA_PROBE_RIME_BINARY_DIR "${PROJECT_BINARY_DIR}")
set(EPOCANVA_PROBE_BOOST_INCLUDE_DIR "${EPOCANVA_BOOST_INCLUDE_DIR}")

# The names are kept in two parallel lists: CMake's foreach over two lists needs
# them positional, and a component that declares no object targets must be
# reported rather than skipped.
set(EPOCANVA_RIME_OBJECT_TARGETS "")
set(EPOCANVA_RIME_COMPONENT_LOG "")
foreach(_epocanva_component ${EPOCANVA_RIME_COMPONENT_NAMES})
  set(_epocanva_dir
      "${EPOCANVA_RIME_COMPONENT_ROOT}/${_epocanva_component}")
  if(NOT EXISTS "${_epocanva_dir}/CMakeLists.txt")
    message(FATAL_ERROR
      "component '${_epocanva_component}' has no CMakeLists.txt at ${_epocanva_dir}")
  endif()
  add_subdirectory("${_epocanva_dir}"
                   "${PROJECT_BINARY_DIR}/epocanva-rime-components/${_epocanva_component}")
  # Contract: every component directory exports epocanva_rime_<name>_object_targets.
  set(_epocanva_declared
      epocanva_rime_${_epocanva_component}_object_targets)
  if(NOT ${_epocanva_declared})
    message(FATAL_ERROR
      "component '${_epocanva_component}' did not declare "
      "${_epocanva_declared}; it would be compiled and then quietly left out of "
      "the engine target.")
  endif()
  list(APPEND EPOCANVA_RIME_OBJECT_TARGETS ${${_epocanva_declared}})
  list(APPEND EPOCANVA_RIME_COMPONENT_LOG "${_epocanva_component}")
  foreach(_epocanva_obj ${${_epocanva_declared}})
    if(NOT TARGET ${_epocanva_obj})
      message(FATAL_ERROR
        "component '${_epocanva_component}' declared ${_epocanva_obj} but no such "
        "target exists after add_subdirectory")
    endif()
  endforeach()
endforeach()

# The attach step must not be able to silently do nothing.
list(LENGTH EPOCANVA_RIME_OBJECT_TARGETS _epocanva_declared_count)
if(_epocanva_declared_count LESS 1)
  message(FATAL_ERROR "no EpoCanva object targets collected from ${EPOCANVA_RIME_COMPONENT_NAMES}")
endif()

function(epocanva_attach_components_to_rime)
  if(TARGET rime)
    set(_engine_target rime)
  elseif(TARGET rime-static)
    # Only reachable if someone configures the static engine; supported rather
    # than silently dropped, because dropping it would produce a DLL without the
    # component while the report claims it was compiled in.
    set(_engine_target rime-static)
  else()
    message(FATAL_ERROR
      "EpoCanva component hook found no engine target to attach to.")
  endif()

  set(_attached "")
  foreach(_objs ${EPOCANVA_RIME_OBJECT_TARGETS})
    if(NOT TARGET ${_objs})
      message(FATAL_ERROR "object target ${_objs} disappeared before the deferred attach")
    endif()
    # The same generator expression upstream uses for plugin objects
    # (plugins/CMakeLists.txt:20 -> src/CMakeLists.txt:35). Object-library objects
    # are passed to the linker unconditionally, unlike static-library members, so
    # they cannot be dropped by "library was never referenced" pruning.
    target_sources(${_engine_target} PRIVATE $<TARGET_OBJECTS:${_objs}>)
    list(APPEND _attached ${_objs})
  endforeach()

  # The build entry asserts this exact line appears in its configure log, so a
  # hook that silently attached nothing cannot be reported as a component build.
  list(LENGTH _attached _count)
  message(STATUS
    "EpoCanva ${EPOCANVA_RIME_HOOK_LABEL} ATTACHED: ${_count} object library(ies) "
    "[${_attached}] -> target ${_engine_target}")
endfunction()

cmake_language(DEFER CALL epocanva_attach_components_to_rime)

message(STATUS "EpoCanva component hook armed for project rime:")
message(STATUS "  label              : ${EPOCANVA_RIME_HOOK_LABEL}")
message(STATUS "  components         : ${EPOCANVA_RIME_COMPONENT_NAMES}")
message(STATUS "  component root     : ${EPOCANVA_RIME_COMPONENT_ROOT}")
message(STATUS "  object targets     : ${EPOCANVA_RIME_OBJECT_TARGETS}")
message(STATUS "  object dir (build) : ${PROJECT_BINARY_DIR}/epocanva-rime-components")
message(STATUS "  upstream tree written: none (all output is under the build tree)")
