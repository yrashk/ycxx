cmake_minimum_required(VERSION 3.30)
project(ycxx_solver_experiments LANGUAGES C CXX)

# Standalone builds for optimizer experiments. The compiler build does not enter this project.
include(ExternalProject)
find_program(YCXX_SOLVER_MAKE NAMES gmake make REQUIRED)
find_package(Threads REQUIRED)

set(YCXX_SOLVER_JOBS 2 CACHE STRING "Parallel jobs per dependency build")
if(NOT YCXX_SOLVER_JOBS MATCHES "^[1-9][0-9]*$")
  message(FATAL_ERROR "YCXX_SOLVER_JOBS must be a positive integer")
endif()
set(solver_prefix "${CMAKE_CURRENT_BINARY_DIR}/install")
set(solver_cmake_args
  -DCMAKE_BUILD_TYPE:STRING=Release
  -DCMAKE_C_COMPILER:FILEPATH=${CMAKE_C_COMPILER}
  -DCMAKE_CXX_COMPILER:FILEPATH=${CMAKE_CXX_COMPILER}
  -DCMAKE_INSTALL_BINDIR:STRING=bin
  -DCMAKE_INSTALL_LIBDIR:STRING=lib)
if(CMAKE_TOOLCHAIN_FILE)
  list(APPEND solver_cmake_args -DCMAKE_TOOLCHAIN_FILE:FILEPATH=${CMAKE_TOOLCHAIN_FILE})
endif()

# These are the precise revisions evaluated in the optimizer design spike.
ExternalProject_Add(highs
  URL https://codeload.github.com/ERGO-Code/HiGHS/tar.gz/f3f113d230429a8c0014b12ab0dd1a3847e58c55
  URL_HASH SHA256=f0448e8fc63db5f3fcba54af42309e8985535fb73f56428444a45f6c8462d4c6
  TLS_VERIFY ON
  INSTALL_DIR "${solver_prefix}/highs"
  CMAKE_ARGS ${solver_cmake_args}
    -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>
    -DBUILD_TESTING:BOOL=OFF
    -DBUILD_SHARED_LIBS:BOOL=OFF
    -DBUILD_SHARED_EXTRAS_LIB:BOOL=OFF
    -DBUILD_EXAMPLES:BOOL=OFF
    -DHIPO:BOOL=OFF
    -DZLIB:BOOL=OFF
    "-DCMAKE_CXX_FLAGS:STRING=-ffp-contract=off"
  BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config Release --parallel ${YCXX_SOLVER_JOBS}
  INSTALL_BYPRODUCTS
    "<INSTALL_DIR>/lib/${CMAKE_STATIC_LIBRARY_PREFIX}highs${CMAKE_STATIC_LIBRARY_SUFFIX}"
    "<INSTALL_DIR>/lib/${CMAKE_STATIC_LIBRARY_PREFIX}highs_extras${CMAKE_STATIC_LIBRARY_SUFFIX}")

# CaDiCaL's configure script requires a build directory directly beneath its source.
ExternalProject_Add(cadical
  URL https://codeload.github.com/arminbiere/cadical/tar.gz/6b3d70b9ce00ecbce3f829bab76d185f0e92618c
  URL_HASH SHA256=b531bb4fef82f7fd6c8741491d06e2ab085b2c725f38dee84b492632a617cc47
  TLS_VERIFY ON
  INSTALL_DIR "${solver_prefix}/cadical"
  PATCH_COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR:PATH=<SOURCE_DIR>
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cadical-posix-time.cmake"
  CONFIGURE_COMMAND ${CMAKE_COMMAND} -E make_directory <SOURCE_DIR>/build
    COMMAND ${CMAKE_COMMAND} -E chdir <SOURCE_DIR>/build
      ${CMAKE_COMMAND} -E env "CC=${CMAKE_C_COMPILER}" "CXX=${CMAKE_CXX_COMPILER}"
      <SOURCE_DIR>/configure --release
  BUILD_COMMAND ${YCXX_SOLVER_MAKE} -C <SOURCE_DIR>/build -j${YCXX_SOLVER_JOBS} libcadical.a cadical
  INSTALL_COMMAND ${CMAKE_COMMAND} -E make_directory <INSTALL_DIR>/bin <INSTALL_DIR>/include <INSTALL_DIR>/lib
    COMMAND ${CMAKE_COMMAND} -E copy <SOURCE_DIR>/build/cadical <INSTALL_DIR>/bin/
    COMMAND ${CMAKE_COMMAND} -E copy <SOURCE_DIR>/build/libcadical.a <INSTALL_DIR>/lib/
    COMMAND ${CMAKE_COMMAND} -E copy
      <SOURCE_DIR>/src/cadical.hpp <SOURCE_DIR>/src/ccadical.h <SOURCE_DIR>/src/tracer.hpp
      <INSTALL_DIR>/include/
  INSTALL_BYPRODUCTS
    "<INSTALL_DIR>/bin/cadical${CMAKE_EXECUTABLE_SUFFIX}"
    "<INSTALL_DIR>/lib/${CMAKE_STATIC_LIBRARY_PREFIX}cadical${CMAKE_STATIC_LIBRARY_SUFFIX}"
    "<INSTALL_DIR>/include/cadical.hpp"
    "<INSTALL_DIR>/include/ccadical.h"
    "<INSTALL_DIR>/include/tracer.hpp")

ExternalProject_Add(chuffed
  URL https://codeload.github.com/chuffed/chuffed/tar.gz/f698d5623bb0c2bcd497388ba966f0436d62caf4
  URL_HASH SHA256=099fcfa9523f109a634c5d14a6575cdcde36a3d76ef6b1304a5cf8c1dcd3c680
  TLS_VERIFY ON
  INSTALL_DIR "${solver_prefix}/chuffed"
  CMAKE_ARGS ${solver_cmake_args}
    -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>
    -DCP_PROFILER:BOOL=OFF
    -DCMAKE_DISABLE_FIND_PACKAGE_ZLIB:BOOL=ON
    -DCMAKE_DISABLE_FIND_PACKAGE_BISON:BOOL=ON
    -DCMAKE_DISABLE_FIND_PACKAGE_FLEX:BOOL=ON
  BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config Release --parallel ${YCXX_SOLVER_JOBS}
  INSTALL_BYPRODUCTS "<INSTALL_DIR>/bin/fzn-chuffed${CMAKE_EXECUTABLE_SUFFIX}")

add_executable(egraph-spike EXCLUDE_FROM_ALL ../../spikes/optimization/egraph/main.cpp)
add_dependencies(egraph-spike highs)
target_compile_features(egraph-spike PRIVATE cxx_std_26)
target_include_directories(egraph-spike SYSTEM PRIVATE "${solver_prefix}/highs/include/highs")
target_link_libraries(egraph-spike PRIVATE
  "${solver_prefix}/highs/lib/${CMAKE_STATIC_LIBRARY_PREFIX}highs${CMAKE_STATIC_LIBRARY_SUFFIX}"
  "${solver_prefix}/highs/lib/${CMAKE_STATIC_LIBRARY_PREFIX}highs_extras${CMAKE_STATIC_LIBRARY_SUFFIX}"
  Threads::Threads ${CMAKE_DL_LIBS})
