# Standalone build of the TSF profile-registration helper for the installer.
# The helper is project-owned and is installed only to register/remove the
# language profile; it is not part of the runtime package manifest.
cmake_minimum_required(VERSION 3.25)
project(EpoCanvaTipProfileHelper LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(EPOCANVA_INSTALLER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

add_executable(epocanva_tip_profile_helper
  "${EPOCANVA_INSTALLER_ROOT}/tools/product/isolation/tip_profile_helper.cc"
  "${EPOCANVA_INSTALLER_ROOT}/platforms/windows/tsf/identifiers.cc")
target_include_directories(epocanva_tip_profile_helper PRIVATE
  "${EPOCANVA_INSTALLER_ROOT}/platforms/windows/tsf")
target_compile_definitions(epocanva_tip_profile_helper PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
if(MSVC)
  target_compile_options(epocanva_tip_profile_helper PRIVATE
    /utf-8 /permissive- /Zc:__cplusplus /W4 /EHsc)
endif()
target_link_libraries(epocanva_tip_profile_helper PRIVATE
  ole32 oleaut32 uuid advapi32)
set_target_properties(epocanva_tip_profile_helper PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
