# Project-side MSVC release-path normalization.
#
# This hook is injected with CMAKE_PROJECT_INCLUDE so it runs in the top-level
# scope of each standalone dependency and of librime itself. The source root is
# derived from this file, not from a machine-specific path. MSVC 19.44 accepts
# /pathmap but warns that it is ignored unless /experimental:deterministic is
# also present; that pair was verified with the CMake probe under
# build/release-paths/probe-src.
#
# /PDBALTPATH names only the CodeView path stored in a PE debug directory. PDB
# files stay in the build tree and are not package inputs.

if(NOT MSVC)
  message(FATAL_ERROR
    "EpoCanva release-path hook requires the MSVC compiler used by the Windows "
    "librime recipe; refusing to claim path normalization for another compiler.")
endif()

get_filename_component(_epocanva_release_repo
  "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
file(TO_CMAKE_PATH "${_epocanva_release_repo}" _epocanva_release_repo_cmake)

# Keep this as an option list rather than concatenating CMAKE_*_FLAGS. CMake then
# preserves the quoted /pathmap argument when the repository path contains spaces.
add_compile_options(
  /experimental:deterministic
  "/pathmap:${_epocanva_release_repo_cmake}=EpoCanva")
add_link_options(/PDBALTPATH:epocanva-dependency.pdb)

message(STATUS
  "EpoCanva release paths: /pathmap:${_epocanva_release_repo_cmake}=EpoCanva")
message(STATUS
  "EpoCanva release paths: /PDBALTPATH:epocanva-dependency.pdb")
