ycxxc_unit_test(pp pp_test.cpp ycxxc_pp)
include(GNUInstallDirs)
add_test(NAME unit.pp_relocated_prefix
  COMMAND python3 -B ${CMAKE_CURRENT_SOURCE_DIR}/relocated_prefix.py
          $<TARGET_FILE:ycxxc> ${PROJECT_SOURCE_DIR}/resources
          "${CMAKE_INSTALL_BINDIR}" "${CMAKE_INSTALL_DATADIR}")

# Exercise the same constructor-registrar OBJECT pattern used by cmake/ycxxConfig.cmake.in
# for relocated static SDKs (#1510).
set(_pp_sdk_fixture "${CMAKE_CURRENT_BINARY_DIR}/sdk_paths_fixture")
set(_pp_sdk_res "${_pp_sdk_fixture}/share/ycxx/resources")
set(_pp_sdk_target "${_pp_sdk_fixture}/share/ycxx/target/include")
file(MAKE_DIRECTORY "${_pp_sdk_res}/linux/include" "${_pp_sdk_res}/common/include" "${_pp_sdk_target}")
file(WRITE "${_pp_sdk_res}/linux/include/stddef.h" "#define YCXX_SDK_REG_STDDEF 11\n")
file(WRITE "${_pp_sdk_res}/common/include/stdarg.h" "#define YCXX_SDK_REG_STDARG 1\n")
file(WRITE "${_pp_sdk_target}/ycxx_sdk_registered.hpp" "#define YCXX_SDK_REG_TARGET 22\n")
if(APPLE)
  set(_pp_sdk_ctor_attr "[[gnu::constructor, gnu::used]]")
else()
  set(_pp_sdk_ctor_attr "[[gnu::constructor(200), gnu::used]]")
endif()
set(_pp_sdk_reg_src "${CMAKE_CURRENT_BINARY_DIR}/pp_sdk_paths_registrar.cpp")
string(CONCAT _pp_sdk_reg_content
  "extern \"C\" void ycxx_pp_register_sdk_paths(const char*, const char*) noexcept;\n"
  "namespace {\n"
  "${_pp_sdk_ctor_attr} void ycxx_pp_register_test_sdk_paths() noexcept {\n"
  "  ycxx_pp_register_sdk_paths(\"${_pp_sdk_res}\", \"${_pp_sdk_target}\");\n"
  "}\n"
  "}\n")
file(WRITE "${_pp_sdk_reg_src}" "${_pp_sdk_reg_content}")
add_library(unit_pp_sdk_paths_reg OBJECT "${_pp_sdk_reg_src}")
target_compile_features(unit_pp_sdk_paths_reg PRIVATE cxx_std_11)
set_target_properties(unit_pp_sdk_paths_reg PROPERTIES
  CXX_SCAN_FOR_MODULES OFF
  CXX_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON)
add_executable(unit_pp_sdk_paths sdk_paths_test.cpp $<TARGET_OBJECTS:unit_pp_sdk_paths_reg>)
target_link_libraries(unit_pp_sdk_paths PRIVATE ycxxc_unit ycxxc_pp)
add_test(NAME unit.pp_sdk_paths COMMAND unit_pp_sdk_paths "${_pp_sdk_res}" "${_pp_sdk_target}")
