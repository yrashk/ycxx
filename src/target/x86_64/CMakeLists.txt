add_library(ycxxc_x86_64 ${YCXXC_COMPONENT_TYPE})
target_sources(ycxxc_x86_64 PUBLIC FILE_SET CXX_MODULES FILES encode.cppm x86_64.cppm)
target_link_libraries(ycxxc_x86_64 PUBLIC ycxxc_lower)
