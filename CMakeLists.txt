cmake_minimum_required(VERSION 3.30)

# ycxx is built by GCC 16.2 with -freflection -fcontracts, against libycxx (deps/libycxx) with
# `import std;` (DECISIONS §3.1, §3.3). The compiler is found or provisioned by libycxx's toolchain
# file, which shares its cache with deps/libycxx/tools/toolchain/provision. Clang 23 is an oracle
# only (DECISIONS §2.3) and never builds ycxx. docs/BUILDING.md has the commands.
set(YCXX_COMPILER gcc CACHE STRING "The bootstrap compiler; GCC 16 is the only one (DECISIONS §3.3)" FORCE)
if(NOT CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/deps/libycxx/cmake/ycxx-toolchain.cmake)
endif()

project(ycxx VERSION 0.0.1
        DESCRIPTION "An independent C++26 compiler"
        LANGUAGES C CXX)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16.2)
  message(FATAL_ERROR "ycxx is bootstrapped with GCC >= 16.2 only (DECISIONS §3.3); this is "
                      "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
endif()
if(NOT CMAKE_GENERATOR MATCHES "Ninja")
  message(FATAL_ERROR "ycxx uses C++ modules, which CMake builds only with a Ninja generator")
endif()

# Our options are YCXXC_*: libycxx owns the YCXX_* cache variables.
option(YCXXC_WERROR "Build ycxx with -Werror" ON)
option(YCXXC_BOOTSTRAP_SMOKE "Build the bootstrap-compiler smoke test (tests/bootstrap)" ON)

# libycxx: the static library and the std modules. Its own tests and install rules are not ours.
set(YCXX_MODULES ON CACHE BOOL "" FORCE)
set(YCXX_WERROR OFF CACHE BOOL "" FORCE)
add_subdirectory(deps/libycxx EXCLUDE_FROM_ALL SYSTEM)
# BOOTSTRAP: modules: the std module's interface is compiled with ycxx::modules' own flags, not
# the importer's. Without -freflection its std::meta is empty, and GCC 16.2 segfaults importing
# it with -freflection mismatched (STATUS, "Bootstrap workarounds").
target_compile_options(ycxx_modules PRIVATE -freflection -fcontracts)

# Every ycxx target: C++26 against libycxx, `import std;`, reflection and contracts. Our targets
# are named ycxxc*, since libycxx owns the ycxx* names.
add_library(ycxxc_options INTERFACE)
target_link_libraries(ycxxc_options INTERFACE ycxx::modules)
target_compile_features(ycxxc_options INTERFACE cxx_std_26)
target_compile_options(ycxxc_options INTERFACE
  -freflection -fcontracts
  -Wall -Wextra -Wpedantic
  $<$<BOOL:${YCXXC_WERROR}>:-Werror>)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)

include(cmake/ycxx-policy.cmake)

add_subdirectory(src)
enable_testing()
if(YCXXC_BOOTSTRAP_SMOKE)
  add_subdirectory(tests/bootstrap)
endif()
add_subdirectory(tests/unit)
