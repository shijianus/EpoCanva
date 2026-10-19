add_executable(epocanva_toolchain_probe main.cc)
target_link_libraries(epocanva_toolchain_probe PRIVATE epocanva_strict)

set_target_properties(epocanva_toolchain_probe PROPERTIES
  OUTPUT_NAME "epocanva_toolchain_probe"
  FOLDER "tools"
)
