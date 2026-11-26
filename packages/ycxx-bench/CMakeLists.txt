cmake_minimum_required(VERSION 3.30)
project(YCXXBench VERSION 0.1.0 DESCRIPTION "Adaptive C++26 benchmarking" LANGUAGES CXX)

if(NOT CMAKE_GENERATOR MATCHES "^Ninja")
  message(FATAL_ERROR "YCXXBench requires a Ninja generator for C++ modules.")
endif()
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16.2)
  message(FATAL_ERROR "YCXXBench requires GCC 16.2 or later for C++26 modules and contracts.")
endif()
if(NOT TARGET ycxx::modules)
  find_package(libycxx 0.1 CONFIG REQUIRED)
endif()
if(NOT TARGET ycxx::modules)
  message(FATAL_ERROR "Build libycxx with YCXX_MODULES=ON to provide ycxx::modules.")
endif()

add_library(ycxx_bench STATIC)
add_library(YCXXBench::bench ALIAS ycxx_bench)
set_target_properties(ycxx_bench PROPERTIES
  EXPORT_NAME bench
  CXX_EXTENSIONS OFF
  CXX_SCAN_FOR_MODULES ON)
target_sources(ycxx_bench PUBLIC FILE_SET CXX_MODULES FILES bench.cppm experiments.cppm)
target_sources(ycxx_bench PRIVATE bench.cpp)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  target_sources(ycxx_bench PRIVATE clock_darwin.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(ycxx_bench PRIVATE clock_linux.cpp)
else()
  message(FATAL_ERROR "YCXXBench has native clocks for Linux and macOS.")
endif()
if(DEFINED YCXX_BENCH_OPTIONS_TARGET)
  target_link_libraries(ycxx_bench PUBLIC "${YCXX_BENCH_OPTIONS_TARGET}")
else()
  target_link_libraries(ycxx_bench PUBLIC ycxx::modules)
  target_compile_features(ycxx_bench PUBLIC cxx_std_26)
  target_compile_options(ycxx_bench PUBLIC -fcontracts)
endif()

add_executable(ycxx-bench-demo demo.cpp)
target_link_libraries(ycxx-bench-demo PRIVATE YCXXBench::bench)
set_target_properties(ycxx-bench-demo PROPERTIES
  CXX_EXTENSIONS OFF
  CXX_SCAN_FOR_MODULES ON
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(NOT PROJECT_IS_TOP_LEVEL)
  set_target_properties(ycxx-bench-demo PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()

if(PROJECT_IS_TOP_LEVEL)
  include(GNUInstallDirs)
  include(CMakePackageConfigHelpers)
  set(config_dir "${CMAKE_INSTALL_LIBDIR}/cmake/YCXXBench")
  configure_package_config_file(YCXXBenchConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/YCXXBenchConfig.cmake"
    INSTALL_DESTINATION "${config_dir}")
  write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/YCXXBenchConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}" COMPATIBILITY SameMinorVersion)
  install(TARGETS ycxx_bench EXPORT YCXXBenchTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET CXX_MODULES DESTINATION "${CMAKE_INSTALL_DATADIR}/ycxx-bench/modules")
  install(TARGETS ycxx-bench-demo RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
  install(EXPORT YCXXBenchTargets NAMESPACE YCXXBench:: DESTINATION "${config_dir}"
    CXX_MODULES_DIRECTORY cxx-modules)
  install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/YCXXBenchConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/YCXXBenchConfigVersion.cmake"
    DESTINATION "${config_dir}")
  install(FILES LICENSE LICENSE-FLYOLOGY DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/YCXXBench")
  install(FILES README.md GUIDE.md DESTINATION "${CMAKE_INSTALL_DOCDIR}")
endif()
