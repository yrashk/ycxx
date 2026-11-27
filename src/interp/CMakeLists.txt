# The L1 interpreter (#25): constant evaluation and whole programs.
add_library(ycxxc_interp ${YCXXC_COMPONENT_TYPE})
target_sources(ycxxc_interp PUBLIC FILE_SET CXX_MODULES FILES
  reason.cppm
  machine.cppm
  interp.cppm)
target_sources(ycxxc_interp PRIVATE ops.cpp)
target_link_libraries(ycxxc_interp PUBLIC ycxxc_ir ycxxc_engine)
