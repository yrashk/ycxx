# ycxx.stack: how much of this thread's stack is in use (#187). The :bounds partition has one file
# per platform.
if(APPLE)
  set(YCXXC_STACK_BOUNDS bounds_darwin.cppm)
else()
  set(YCXXC_STACK_BOUNDS bounds_linux.cppm)
endif()
add_library(ycxxc_stack STATIC)
target_sources(ycxxc_stack PUBLIC FILE_SET CXX_MODULES FILES ${YCXXC_STACK_BOUNDS} stack.cppm)
target_link_libraries(ycxxc_stack PUBLIC ycxxc_options)
