cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(EpoCanvaFrenchDictionary LANGUAGES CXX)
endif()

# This scoped target never configures upstream's project, installs, downloads,
# mutates third_party/, or changes the embedding project's CRT selection.
get_filename_component(EPOCANVA_FR_REPO "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
file(REAL_PATH "${EPOCANVA_FR_REPO}/build" _fr_build_root)
file(REAL_PATH "${CMAKE_BINARY_DIR}" _fr_binary_root)
string(FIND "${_fr_binary_root}/" "${_fr_build_root}/" _fr_build_prefix)
if(NOT _fr_build_prefix EQUAL 0)
  message(FATAL_ERROR "French dictionary build artifacts must stay under repository build/<preset>/.")
endif()
if(NOT WIN32 OR NOT MSVC)
  message(FATAL_ERROR "This native backend wrapper currently requires Windows/MSVC (UTF-16 local path locking and BCrypt SHA-256).")
endif()

set(_fr_manifest "${EPOCANVA_FR_REPO}/data/manifests/backend-hunspell.sources.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_fr_manifest}")
file(READ "${_fr_manifest}" _fr_json)
string(JSON _fr_revision GET "${_fr_json}" hunspell revision)
string(JSON _fr_archive_root GET "${_fr_json}" hunspell archive_root)
string(JSON _fr_archive_hash GET "${_fr_json}" hunspell archive_sha256)
set(EPOCANVA_HUNSPELL_ROOT
  "${EPOCANVA_FR_REPO}/build/backend-french-dependency/${_fr_archive_root}"
  CACHE PATH "Prepared, byte-clean Hunspell source; no CMake network acquisition")
get_filename_component(_fr_dependency_parent "${EPOCANVA_HUNSPELL_ROOT}" DIRECTORY)
set(_fr_archive "${_fr_dependency_parent}/downloads/${_fr_archive_root}.tar.gz")
if(NOT EXISTS "${_fr_archive}")
  message(FATAL_ERROR "Pinned Hunspell archive unavailable. Run tools/backend/prepare_french.ps1 -Download explicitly, then configure again.")
endif()
file(SHA256 "${_fr_archive}" _fr_actual_archive_hash)
if(NOT _fr_actual_archive_hash STREQUAL _fr_archive_hash)
  message(FATAL_ERROR "Hunspell archive SHA-256 mismatch; configuration is offline and will not replace it.")
endif()
if(EXISTS "${EPOCANVA_HUNSPELL_ROOT}/.git")
  message(FATAL_ERROR "Hunspell must be a verified source archive, not a nested Git repository.")
endif()
set(_fr_native_sources)
foreach(_fr_group library_files license_files)
  string(JSON _fr_count LENGTH "${_fr_json}" hunspell ${_fr_group})
  math(EXPR _fr_last "${_fr_count} - 1")
  foreach(_fr_ordinal RANGE 0 ${_fr_last})
    string(JSON _fr_path GET "${_fr_json}" hunspell ${_fr_group} ${_fr_ordinal} path)
    string(JSON _fr_hash GET "${_fr_json}" hunspell ${_fr_group} ${_fr_ordinal} sha256)
    set(_fr_file "${EPOCANVA_HUNSPELL_ROOT}/${_fr_path}")
    if(NOT EXISTS "${_fr_file}")
      message(FATAL_ERROR "Prepared Hunspell file unavailable: ${_fr_path}")
    endif()
    file(SHA256 "${_fr_file}" _fr_actual_hash)
    if(NOT _fr_actual_hash STREQUAL _fr_hash)
      message(FATAL_ERROR "Prepared Hunspell file SHA-256 mismatch: ${_fr_path}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_fr_file}")
    if(_fr_group STREQUAL "library_files")
      list(APPEND _fr_native_sources "${_fr_file}")
    endif()
  endforeach()
endforeach()

add_library(epocanva_hunspell STATIC ${_fr_native_sources})
target_include_directories(epocanva_hunspell PRIVATE "${EPOCANVA_HUNSPELL_ROOT}/src/hunspell")
target_compile_definitions(epocanva_hunspell PRIVATE HUNSPELL_STATIC BUILDING_LIBHUNSPELL
  _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
# Explicitly keep HUNSPELL_WARNING_ON off. It can print raw query text from
# csutil.cxx on malformed UTF-8; the wrapper additionally validates before calls.
target_compile_options(epocanva_hunspell PRIVATE /utf-8 /permissive- /EHsc
  /Zc:__cplusplus /W3 /UHUNSPELL_WARNING_ON)
set_target_properties(epocanva_hunspell PROPERTIES CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON
  FOLDER "epocanva/french_dictionary")

add_library(epocanva_french_dictionary STATIC dictionary.cc dictionary.h)
target_include_directories(epocanva_french_dictionary PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}" "${EPOCANVA_FR_REPO}")
target_include_directories(epocanva_french_dictionary PRIVATE
  "${EPOCANVA_HUNSPELL_ROOT}/src/hunspell")
target_compile_definitions(epocanva_french_dictionary PRIVATE HUNSPELL_STATIC
  WINVER=0x0A00 _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(epocanva_french_dictionary PRIVATE /utf-8 /permissive-
  /EHsc /Zc:__cplusplus /W4 /WX /UHUNSPELL_WARNING_ON)
target_compile_features(epocanva_french_dictionary PUBLIC cxx_std_17)
target_link_libraries(epocanva_french_dictionary PRIVATE epocanva_hunspell bcrypt)
set_target_properties(epocanva_french_dictionary PROPERTIES CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON
  FOLDER "epocanva/french_dictionary")
