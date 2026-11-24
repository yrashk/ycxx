add_library(ycxxc_build_manifest STATIC)
target_sources(ycxxc_build_manifest PUBLIC FILE_SET CXX_MODULES FILES manifest.cppm)
target_link_libraries(ycxxc_build_manifest PUBLIC ycxxc_options)

add_subdirectory(action)
