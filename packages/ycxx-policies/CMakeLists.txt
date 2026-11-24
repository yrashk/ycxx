cmake_minimum_required(VERSION 3.20)
project(YCXXPolicies VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(YCXX_POLICIES_RUNTIME_CHECKS "Enable YCXX Policies runtime guards" ON)

add_library(ycxx_policies INTERFACE)
add_library(YCXXPolicies::policies ALIAS ycxx_policies)
set_target_properties(ycxx_policies PROPERTIES EXPORT_NAME policies)
target_compile_features(ycxx_policies INTERFACE cxx_std_20)
if(NOT YCXX_POLICIES_RUNTIME_CHECKS)
  target_compile_definitions(ycxx_policies INTERFACE YCXX_POLICIES_RUNTIME_CHECKS=0)
endif()
target_include_directories(ycxx_policies INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

set(ycxx_policies_config_dir ${CMAKE_INSTALL_LIBDIR}/cmake/YCXXPolicies)
configure_package_config_file(YCXXPoliciesConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/YCXXPoliciesConfig.cmake
  INSTALL_DESTINATION ${ycxx_policies_config_dir})
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/YCXXPoliciesConfigVersion.cmake
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion ARCH_INDEPENDENT)

install(TARGETS ycxx_policies EXPORT YCXXPoliciesTargets)
install(EXPORT YCXXPoliciesTargets NAMESPACE YCXXPolicies:: DESTINATION ${ycxx_policies_config_dir})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/YCXXPoliciesConfig.cmake
              ${CMAKE_CURRENT_BINARY_DIR}/YCXXPoliciesConfigVersion.cmake
  DESTINATION ${ycxx_policies_config_dir})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/YCXXPolicies)
