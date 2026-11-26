include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

function(ycxxc_component_targets directory output)
  get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
  set(components)
  foreach(target IN LISTS targets)
    get_target_property(type "${target}" TYPE)
    if(target MATCHES "^ycxxc_" AND type MATCHES "^(STATIC|SHARED)_LIBRARY$")
      list(APPEND components "${target}")
    endif()
  endforeach()
  get_property(subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
  foreach(subdirectory IN LISTS subdirectories)
    ycxxc_component_targets("${subdirectory}" children)
    list(APPEND components ${children})
  endforeach()
  set(${output} "${components}" PARENT_SCOPE)
endfunction()

ycxxc_component_targets("${PROJECT_SOURCE_DIR}/src" ycxxc_components)
install(DIRECTORY resources/ DESTINATION "${CMAKE_INSTALL_DATADIR}/ycxx/resources")
# These files sit beside installed module interfaces that embed them when a consumer builds BMIs.
install(FILES src/lex/unicode_names.txt DESTINATION "${CMAKE_INSTALL_DATADIR}/ycxx/modules/ycxxc_lex")
install(FILES src/target/aarch64/aarch64.patterns
  DESTINATION "${CMAKE_INSTALL_DATADIR}/ycxx/modules/ycxxc_aarch64")

install(DIRECTORY "${YCXXC_LIBYCXX_SOURCE_DIR}/include/"
                  "${PROJECT_BINARY_DIR}/deps/libycxx/generated/include/"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/ycxx/target/include")

# These archives are inputs for programs the SDK compiles, separate from host link dependencies.
get_property(ycxxc_darwin_archives TARGET ycxxc_pipeline PROPERTY YCXXC_DARWIN_RUNTIME_ARCHIVES)
get_property(ycxxc_darwin_undefined TARGET ycxxc_pipeline PROPERTY YCXXC_DARWIN_RUNTIME_UNDEFINED)
set(ycxxc_darwin_archive_names)
if(ycxxc_darwin_archives)
  install(FILES ${ycxxc_darwin_archives}
    DESTINATION "${CMAKE_INSTALL_DATADIR}/ycxx/target/arm64-apple-darwin/lib")
  foreach(archive IN LISTS ycxxc_darwin_archives)
    cmake_path(GET archive FILENAME archive_name)
    list(APPEND ycxxc_darwin_archive_names "${archive_name}")
  endforeach()
endif()
set(ycxxc_cmake_directory "${CMAKE_INSTALL_LIBDIR}/cmake/ycxx")

# The dependency subdirectories are excluded from the normal build and install. An SDK
# installation includes the exact providers used to build these libraries.
if(YCXXC_STDLIB STREQUAL "system")
  install(SCRIPT "${PROJECT_BINARY_DIR}/src/execution/libycxx-execution/cmake_install.cmake")
else()
  add_custom_target(ycxxc_runtime_dependencies ALL)
  foreach(runtime IN ITEMS ycxx ycxx_abi ycxx_so ycxx_nonshared ycxx_modules ycxx_shared_modules ycxx_freestanding)
    if(TARGET "${runtime}")
      add_dependencies(ycxxc_runtime_dependencies "${runtime}")
    endif()
  endforeach()
  install(SCRIPT "${PROJECT_BINARY_DIR}/deps/libycxx/cmake_install.cmake")
endif()

set(ycxxc_exported_components)
foreach(target IN LISTS ycxxc_components)
  get_target_property(export_name "${target}" EXPORT_NAME)
  if(NOT export_name)
    set(export_name "${target}")
  endif()
  list(APPEND ycxxc_exported_components "ycxx::${export_name}")
  if(YCXXC_BUILD_SHARED_LIBS)
    if(APPLE)
      set_property(TARGET "${target}" PROPERTY INSTALL_RPATH "@loader_path")
    else()
      set_property(TARGET "${target}" PROPERTY INSTALL_RPATH "$ORIGIN")
    endif()
  endif()
  install(TARGETS "${target}" EXPORT ycxxTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET CXX_MODULES DESTINATION "${CMAKE_INSTALL_DATADIR}/ycxx/modules/${target}")
endforeach()
install(TARGETS ycxxc_options EXPORT ycxxTargets)
install(EXPORT ycxxTargets NAMESPACE ycxx:: DESTINATION "${ycxxc_cmake_directory}"
  CXX_MODULES_DIRECTORY cxx-modules)
configure_package_config_file(cmake/ycxxConfig.cmake.in "${PROJECT_BINARY_DIR}/ycxxConfig.cmake"
  INSTALL_DESTINATION "${ycxxc_cmake_directory}" PATH_VARS CMAKE_INSTALL_DATADIR)
write_basic_package_version_file("${PROJECT_BINARY_DIR}/ycxxConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}" COMPATIBILITY ExactVersion)
install(FILES "${PROJECT_BINARY_DIR}/ycxxConfig.cmake" "${PROJECT_BINARY_DIR}/ycxxConfigVersion.cmake"
  cmake/ycxx-system-stdlib.cmake DESTINATION "${ycxxc_cmake_directory}")
install(FILES LICENSE DESTINATION "${CMAKE_INSTALL_DATADIR}/ycxx")
