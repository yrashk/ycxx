add_library(ycxxc_cm ${YCXXC_COMPONENT_TYPE})
target_sources(ycxxc_cm PUBLIC FILE_SET CXX_MODULES FILES cm.cppm)
target_link_libraries(ycxxc_cm PUBLIC ycxxc_execution)
