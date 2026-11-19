# ycxx.memory: what the process allocates, counted per process, per thread and per engine frame
# (#274). The :usable partition has one file per platform.
if(APPLE)
  set(YCXXC_MEMORY_USABLE usable_darwin.cppm)
else()
  set(YCXXC_MEMORY_USABLE usable_linux.cppm)
endif()
add_library(ycxxc_memory STATIC)
target_sources(ycxxc_memory PUBLIC FILE_SET CXX_MODULES FILES ${YCXXC_MEMORY_USABLE} memory.cppm)
target_link_libraries(ycxxc_memory PUBLIC ycxxc_options)
# The replacements of the allocation functions, linked into each program that counts (`ycxx`, and
# the unit tests that check memory). Optimized in every build type: every allocation runs it.
add_library(ycxxc_new OBJECT new.cpp)
target_link_libraries(ycxxc_new PUBLIC ycxxc_memory)
set_source_files_properties(new.cpp PROPERTIES COMPILE_OPTIONS "-O2")
