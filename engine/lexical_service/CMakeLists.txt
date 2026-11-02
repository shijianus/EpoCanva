# May be used directly or from an aggregate backend build. Existing language
# targets are reused; upstream/core files are never edited or copied here.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.25)
  project(EpoCanvaLexicalService LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
endif()
get_filename_component(EPOCANVA_LEXICAL_REPO "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  file(REAL_PATH "${EPOCANVA_LEXICAL_REPO}/build" _lexical_build_root)
  file(REAL_PATH "${CMAKE_BINARY_DIR}" _lexical_binary_root)
  if(WIN32)
    string(TOLOWER "${_lexical_build_root}" _lexical_build_root)
    string(TOLOWER "${_lexical_binary_root}" _lexical_binary_root)
  endif()
  string(FIND "${_lexical_binary_root}/" "${_lexical_build_root}/" _lexical_build_prefix)
  if(NOT _lexical_build_prefix EQUAL 0 OR _lexical_binary_root STREQUAL _lexical_build_root)
    message(FATAL_ERROR "Lexical artifacts must stay under build/<preset>/.")
  endif()
endif()

if(NOT TARGET epocanva_en_core)
  if(TARGET epocanva_en_core_objs)
    add_library(epocanva_en_core STATIC $<TARGET_OBJECTS:epocanva_en_core_objs>)
    target_include_directories(epocanva_en_core PUBLIC "${EPOCANVA_LEXICAL_REPO}/engine/languages/en")
  else()
    set(EPOCANVA_EN_BUILD_STANDALONE ON)
    add_subdirectory("${EPOCANVA_LEXICAL_REPO}/engine/languages/en"
                     "${CMAKE_CURRENT_BINARY_DIR}/en-core")
  endif()
endif()
if(NOT TARGET epocanva_fr_core)
  add_library(epocanva_fr_core STATIC
    "${EPOCANVA_LEXICAL_REPO}/engine/languages/fr/french_core.cc")
  target_include_directories(epocanva_fr_core PUBLIC
    "${EPOCANVA_LEXICAL_REPO}/engine/languages/fr")
  set_target_properties(epocanva_fr_core PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  if(MSVC)
    target_compile_options(epocanva_fr_core PRIVATE /W4 /permissive- /utf-8 /EHsc)
  else()
    target_compile_options(epocanva_fr_core PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endif()
if(NOT TARGET epocanva_prediction_core)
  add_subdirectory("${EPOCANVA_LEXICAL_REPO}/engine/prediction"
                   "${CMAKE_CURRENT_BINARY_DIR}/prediction")
endif()

add_library(epocanva_lexical_service STATIC text.cc lexical_service.cc)
target_include_directories(epocanva_lexical_service PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(epocanva_lexical_service PUBLIC
  epocanva_en_core epocanva_fr_core epocanva_prediction_core)
target_compile_features(epocanva_lexical_service PUBLIC cxx_std_17)
set_target_properties(epocanva_lexical_service PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
if(WIN32)
  # NormalizeString is provided by the Windows SDK Normaliz import library.
  # LCMapStringEx/GetStringTypeW/strict UTF conversion are system NLS calls.
  target_link_libraries(epocanva_lexical_service PRIVATE Normaliz)
endif()
if(MSVC)
  target_compile_options(epocanva_lexical_service PRIVATE /W4 /WX /permissive- /utf-8 /EHsc)
else()
  target_compile_options(epocanva_lexical_service PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
