# ycxx.parse: the parser and the per-item queries item_sema, decl_sema and analyze_body (#41).
add_library(ycxxc_parse ${YCXXC_COMPONENT_TYPE})
target_sources(ycxxc_parse PUBLIC FILE_SET CXX_MODULES FILES parse.cppm parser.cppm)
target_link_libraries(ycxxc_parse PUBLIC ycxxc_sema ycxxc_items ycxxc_pp ycxxc_irgen ycxxc_interp ycxxc_stack)
