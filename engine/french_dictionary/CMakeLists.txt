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
set(_fr_verified_paths)
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
      list(APPEND _fr_verified_paths "${_fr_path}")
    endif()
  endforeach()
endforeach()

# Deterministic-candidate patch. The upstream archive is never edited in place:
# a verified copy is made under this build tree and the pinned patch is applied
# with `git apply`, so the pristine dependency stays byte-identical and the
# modification is reproducible from the manifest's patch hash.
string(JSON _fr_patch_path GET "${_fr_json}" hunspell patch path)
string(JSON _fr_patch_hash GET "${_fr_json}" hunspell patch sha256)
string(JSON _fr_patch_bytes GET "${_fr_json}" hunspell patch bytes)
set(_fr_patch "${EPOCANVA_FR_REPO}/${_fr_patch_path}")
if(NOT EXISTS "${_fr_patch}")
  message(FATAL_ERROR "Pinned Hunspell patch is unavailable: ${_fr_patch_path}")
endif()
file(SHA256 "${_fr_patch}" _fr_actual_patch_hash)
file(SIZE "${_fr_patch}" _fr_actual_patch_bytes)
if(NOT _fr_actual_patch_hash STREQUAL _fr_patch_hash OR
   NOT _fr_actual_patch_bytes EQUAL _fr_patch_bytes)
  message(FATAL_ERROR "Hunspell patch SHA-256/byte count mismatch.")
endif()
set(_fr_patched_root "${CMAKE_CURRENT_BINARY_DIR}/hunspell-patched")
set(_fr_patch_stamp "${CMAKE_CURRENT_BINARY_DIR}/hunspell-patched.stamp")
set(_fr_needs_patch TRUE)
if(EXISTS "${_fr_patch_stamp}" AND EXISTS "${_fr_patched_root}/src/hunspell/atypes.hxx")
  file(READ "${_fr_patch_stamp}" _fr_stamp_hash)
  string(STRIP "${_fr_stamp_hash}" _fr_stamp_hash)
  if(_fr_stamp_hash STREQUAL _fr_patch_hash)
    set(_fr_needs_patch FALSE)
  endif()
endif()
if(_fr_needs_patch)
  file(REMOVE_RECURSE "${_fr_patched_root}")
  file(MAKE_DIRECTORY "${_fr_patched_root}")
  file(COPY "${EPOCANVA_HUNSPELL_ROOT}/src" DESTINATION "${_fr_patched_root}")
  file(RELATIVE_PATH _fr_patched_rel "${EPOCANVA_FR_REPO}" "${_fr_patched_root}")
  file(TO_CMAKE_PATH "${_fr_patched_rel}" _fr_patched_rel)
  execute_process(
    COMMAND git -C "${EPOCANVA_FR_REPO}" apply --whitespace=nowarn
            "--directory=${_fr_patched_rel}" "${_fr_patch}"
    RESULT_VARIABLE _fr_patch_result
    OUTPUT_VARIABLE _fr_patch_stdout
    ERROR_VARIABLE _fr_patch_stderr)
  if(NOT _fr_patch_result EQUAL 0)
    message(FATAL_ERROR "Applying the verified Hunspell patch failed (exit ${_fr_patch_result}): ${_fr_patch_stderr}")
  endif()
  foreach(_fr_name hunspell.cxx affixmgr.cxx suggestmgr.cxx)
    file(READ "${_fr_patched_root}/src/hunspell/${_fr_name}" _fr_patched_text)
    string(FIND "${_fr_patched_text}" "std::chrono::steady_clock::now()" _fr_wall_clock)
    if(NOT _fr_wall_clock EQUAL -1)
      message(FATAL_ERROR "Hunspell patch left a wall-clock suggestion cutoff: ${_fr_name}")
    endif()
    string(FIND "${_fr_patched_text}" "epocanva_det_clock::now()" _fr_det_clock)
    if(_fr_det_clock EQUAL -1)
      message(FATAL_ERROR "Hunspell patch did not install the deterministic clock: ${_fr_name}")
    endif()
  endforeach()
  file(WRITE "${_fr_patch_stamp}" "${_fr_patch_hash}\n")
endif()

set(_fr_native_sources)
foreach(_fr_path IN LISTS _fr_verified_paths)
  list(APPEND _fr_native_sources "${_fr_patched_root}/${_fr_path}")
endforeach()

add_library(epocanva_hunspell STATIC ${_fr_native_sources})
target_include_directories(epocanva_hunspell PRIVATE "${_fr_patched_root}/src/hunspell")
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
  "${_fr_patched_root}/src/hunspell")
target_compile_definitions(epocanva_french_dictionary PRIVATE HUNSPELL_STATIC
  WINVER=0x0A00 _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(epocanva_french_dictionary PRIVATE /utf-8 /permissive-
  /EHsc /Zc:__cplusplus /W4 /WX /UHUNSPELL_WARNING_ON)
target_compile_features(epocanva_french_dictionary PUBLIC cxx_std_17)
target_link_libraries(epocanva_french_dictionary PRIVATE epocanva_hunspell bcrypt)
set_target_properties(epocanva_french_dictionary PROPERTIES CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON
  FOLDER "epocanva/french_dictionary")
