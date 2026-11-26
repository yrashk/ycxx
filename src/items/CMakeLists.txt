# ycxx.items: the item splitter (#18).
add_library(ycxxc_items ${YCXXC_COMPONENT_TYPE})
target_sources(ycxxc_items PUBLIC FILE_SET CXX_MODULES FILES items.cppm)
target_link_libraries(ycxxc_items PUBLIC ycxxc_pp)
