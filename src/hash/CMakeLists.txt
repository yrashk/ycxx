# ycxx.hash: the 128-bit fingerprint every other module uses (query-engine design §A1.2, #14).
add_library(ycxxc_hash ${YCXXC_COMPONENT_TYPE})
target_sources(ycxxc_hash PUBLIC FILE_SET CXX_MODULES FILES hash.cppm)
target_link_libraries(ycxxc_hash PUBLIC ycxxc_options)
