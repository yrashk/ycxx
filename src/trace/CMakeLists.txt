# ycxx.trace: what went through the query engine, for `ycxx cxx --trace-engine` (#245).
add_library(ycxxc_trace STATIC)
target_sources(ycxxc_trace PUBLIC FILE_SET CXX_MODULES FILES model.cppm view.cppm browse.cppm trace.cppm)
target_link_libraries(ycxxc_trace PUBLIC ycxxc_engine)
