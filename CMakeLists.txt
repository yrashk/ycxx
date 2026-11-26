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
# A sanitizer build (the nightly tier, DECISIONS §9.3; #105): ycxx's targets, the std modules and
# libycxx itself instrumented, so that ThreadSanitizer sees the library's synchronization too.
set(YCXXC_SANITIZE "" CACHE STRING
    "Instrument ycxx and libycxx with -fsanitize=<list> (address, undefined, thread; comma-separated)")

# libycxx: the static library and the std modules. Its own tests and install rules are not ours.
set(YCXX_MODULES ON CACHE BOOL "" FORCE)
set(YCXX_WERROR OFF CACHE BOOL "" FORCE)
set(YCXX_SANITIZE "${YCXXC_SANITIZE}" CACHE STRING "" FORCE)
# BOOTSTRAP: source modules: the scheduler's import std creates a CMake dependency cycle
# with the embedded runtime archive (STATUS, "Bootstrap workarounds").
block(SCOPE_FOR VARIABLES)
  set(YCXX_SOURCE_MODULES OFF)
  add_subdirectory(deps/libycxx EXCLUDE_FROM_ALL SYSTEM)
endblock()
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
if(YCXXC_SANITIZE)
  set(_ycxxc_sanitize -fsanitize=${YCXXC_SANITIZE} -fno-sanitize-recover=all -fno-omit-frame-pointer)
  # GCC warns that ThreadSanitizer does not model atomic_thread_fence (-Wtsan). The engine's
  # seq_cst fences order a store before a later load (the observed flag and a reset's claim, a
  # Dekker pair); happens-before for plain data comes from the atomic operations themselves.
  if(YCXXC_SANITIZE MATCHES "thread")
    list(APPEND _ycxxc_sanitize -Wno-tsan)
  endif()
  target_compile_options(ycxxc_options INTERFACE ${_ycxxc_sanitize})
  target_link_options(ycxxc_options INTERFACE -fsanitize=${YCXXC_SANITIZE})
  target_compile_options(ycxx_modules PRIVATE ${_ycxxc_sanitize})
endif()
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)

include(cmake/ycxx-policy.cmake)

add_subdirectory(packages/ycxx-bench)
# Use the same GCC module dialect and sanitizer settings as the in-tree std module.
target_link_libraries(ycxx_bench PUBLIC ycxxc_options)

add_subdirectory(src)
enable_testing()
if(YCXXC_BOOTSTRAP_SMOKE)
  add_subdirectory(tests/bootstrap)
endif()
add_subdirectory(tests/unit)
add_subdirectory(tests/daemon)
