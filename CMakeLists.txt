cmake_minimum_required(VERSION 3.30)

# GCC 16.2 bootstraps ycxx with reflection, contracts and import std.
# The default runtime is libycxx. The system configuration uses GCC's standard library and
# the portable libycxx execution component. See docs/BUILDING.md for both.
set(YCXX_COMPILER gcc CACHE STRING "The bootstrap compiler; GCC 16 is the only one (DECISIONS §3.3)" FORCE)
set(YCXXC_LIBYCXX_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/libycxx" CACHE PATH
  "libycxx checkout used for the runtime and portable execution provider")
if(NOT CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "${YCXXC_LIBYCXX_SOURCE_DIR}/cmake/ycxx-toolchain.cmake")
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
option(YCXXC_BUILD_TESTS "Build ycxx tests" ${PROJECT_IS_TOP_LEVEL})
option(YCXXC_BOOTSTRAP_SMOKE "Build the bootstrap-compiler smoke test (tests/bootstrap)" ON)
# A sanitizer build (the nightly tier, DECISIONS §9.3; #105): ycxx's targets, the std modules and
# libycxx itself instrumented, so that ThreadSanitizer sees the library's synchronization too.
set(YCXXC_SANITIZE "" CACHE STRING
    "Instrument ycxx and libycxx with -fsanitize=<list> (address, undefined, thread; comma-separated)")

set(YCXXC_STDLIB "libycxx" CACHE STRING "C++ standard library used to build ycxx")
set_property(CACHE YCXXC_STDLIB PROPERTY STRINGS libycxx system)
option(YCXXC_INSTALL "Install the linkable ycxx components and CMake package" OFF)
option(YCXXC_BUILD_SHARED_LIBS "Build ycxx components as shared libraries" OFF)
set(YCXXC_COMPONENT_TYPE STATIC)
if(YCXXC_BUILD_SHARED_LIBS)
  set(YCXXC_COMPONENT_TYPE SHARED)
endif()
if(NOT YCXXC_STDLIB MATCHES "^(libycxx|system)$")
  message(FATAL_ERROR "YCXXC_STDLIB must be libycxx or system")
endif()

# Even a system-standard-library build uses libycxx as a runtime for its Darwin target.
set(YCXX_MODULES ON CACHE BOOL "" FORCE)
set(YCXX_WERROR OFF CACHE BOOL "" FORCE)
set(YCXX_SANITIZE "${YCXXC_SANITIZE}" CACHE STRING "" FORCE)
block(SCOPE_FOR VARIABLES)
  # BOOTSTRAP: source modules: the scheduler's import std creates a dependency cycle.
  set(YCXX_SOURCE_MODULES OFF)
  if(YCXXC_INSTALL AND YCXXC_STDLIB STREQUAL "libycxx")
    set(YCXX_INSTALL ON)
    set(YCXX_MODULES_REFLECTION ON)
  endif()
  if(YCXXC_BUILD_SHARED_LIBS AND YCXXC_STDLIB STREQUAL "libycxx")
    set(YCXX_SHARED ON)
    set(YCXX_STATIC ON)
    set(YCXX_MODULES_REFLECTION ON)
  endif()
  add_subdirectory("${YCXXC_LIBYCXX_SOURCE_DIR}" deps/libycxx EXCLUDE_FROM_ALL SYSTEM)
endblock()

add_library(ycxxc_options INTERFACE)
if(YCXXC_STDLIB STREQUAL "system")
  include(cmake/ycxx-system-stdlib.cmake)
  ycxxc_system_std_module()
  target_link_libraries(ycxxc_options INTERFACE
    "$<BUILD_INTERFACE:ycxxc_system_std>"
    "$<INSTALL_INTERFACE:ycxx::system_std>")
  # GCC adds its experimental standard-library archive to the link with -fcontracts.
  target_link_options(ycxxc_options INTERFACE -fcontracts)
elseif(YCXXC_BUILD_SHARED_LIBS)
  set(YCXXC_STDLIB_MODULE ycxx_shared_modules)
  target_link_libraries(ycxxc_options INTERFACE ycxx::shared_modules)
else()
  set(YCXXC_STDLIB_MODULE ycxx_modules)
  target_link_libraries(ycxxc_options INTERFACE ycxx::modules)
endif()
if(YCXXC_STDLIB_MODULE)
  # Build std's BMI with the same reflection and contracts flags as its importers.
  target_compile_options(${YCXXC_STDLIB_MODULE} PRIVATE -freflection -fcontracts)
endif()
target_compile_features(ycxxc_options INTERFACE cxx_std_26)
target_compile_options(ycxxc_options INTERFACE
  -freflection -fcontracts
  "$<BUILD_INTERFACE:-Wall;-Wextra;-Wpedantic;$<$<BOOL:${YCXXC_WERROR}>:-Werror>>")
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
  if(YCXXC_STDLIB_MODULE)
    target_compile_options(${YCXXC_STDLIB_MODULE} PRIVATE ${_ycxxc_sanitize})
  endif()
endif()
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)

include(cmake/ycxx-policy.cmake)

block(SCOPE_FOR VARIABLES)
  # The package and its consumers import the same std module as the rest of ycxx.
  set(YCXX_BENCH_OPTIONS_TARGET ycxxc_options)
  add_subdirectory(packages/ycxx-bench)
endblock()

add_subdirectory(src)
if(YCXXC_INSTALL)
  include(cmake/ycxx-install.cmake)
endif()
if(YCXXC_BUILD_TESTS)
  enable_testing()
  if(YCXXC_BOOTSTRAP_SMOKE)
    add_subdirectory(tests/bootstrap)
  endif()
  add_subdirectory(tests/unit)
  add_subdirectory(tests/daemon)
  add_subdirectory(examples/embedding)
endif()
