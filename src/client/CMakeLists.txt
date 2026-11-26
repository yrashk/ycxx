# The c++-compatible request handler (ycxx.compile), shared by the daemon and the in-process path.
add_library(ycxxc_compile ${YCXXC_COMPONENT_TYPE})
target_sources(ycxxc_compile PUBLIC FILE_SET CXX_MODULES FILES compile.cppm)
target_link_libraries(ycxxc_compile PUBLIC ycxxc_pipeline)
