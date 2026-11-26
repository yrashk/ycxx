# ycxx.trace: what went through the query engine, for `ycxx cxx --trace-engine` (#245). The
# :clipboard partition has one file per platform (#636).
if(APPLE)
  set(YCXXC_TRACE_CLIPBOARD clipboard_darwin.cppm)
else()
  set(YCXXC_TRACE_CLIPBOARD clipboard_other.cppm)
endif()
add_library(ycxxc_trace ${YCXXC_COMPONENT_TYPE})
target_sources(ycxxc_trace PUBLIC FILE_SET CXX_MODULES FILES model.cppm view.cppm ${YCXXC_TRACE_CLIPBOARD} browse.cppm trace.cppm)
target_link_libraries(ycxxc_trace PUBLIC ycxxc_engine ${CMAKE_DL_LIBS})
