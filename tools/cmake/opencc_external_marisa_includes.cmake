# Included into OpenCC's top-level project scope by CMake's documented
# CMAKE_PROJECT_<projectName>_INCLUDE hook. EpoCanva uses it for two things that
# OpenCC only performs for its *bundled* marisa:
#
#   1. the header directory -- deps/opencc/src/CMakeLists.txt:6-8 adds
#      ../deps/marisa-0.2.6/include only in the USE_SYSTEM_MARISA=OFF branch, so
#      with an external marisa nothing supplies marisa.h;
#   2. the library directory -- deps/opencc/src/CMakeLists.txt:118 links the bare
#      name `marisa`, which resolves to a target in the bundled case and to an
#      unsearchable file name in the external case.
#
# Doing it here keeps the upstream checkout byte-clean; the effect is the same as
# the include_directories()/link_directories() calls the bundled branch gets.

if(NOT DEFINED EPOCANVA_MARISA_ROOT OR EPOCANVA_MARISA_ROOT STREQUAL "")
  message(FATAL_ERROR
    "EPOCANVA_MARISA_ROOT must point at a directory containing include/ and lib/")
endif()

get_filename_component(_epocanva_marisa_include "${EPOCANVA_MARISA_ROOT}/include" ABSOLUTE)
get_filename_component(_epocanva_marisa_lib "${EPOCANVA_MARISA_ROOT}/lib" ABSOLUTE)

if(NOT EXISTS "${_epocanva_marisa_include}/marisa.h")
  message(FATAL_ERROR
    "no marisa.h under ${_epocanva_marisa_include}")
endif()
if(NOT EXISTS "${_epocanva_marisa_lib}/marisa.lib")
  message(FATAL_ERROR
    "no marisa.lib under ${_epocanva_marisa_lib}")
endif()

include_directories("${_epocanva_marisa_include}")
link_directories("${_epocanva_marisa_lib}")
message(STATUS
  "EpoCanva external-marisa hook: includes ${_epocanva_marisa_include}; "
  "link dirs ${_epocanva_marisa_lib}")
