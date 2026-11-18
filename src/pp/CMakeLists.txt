# ycxx.pp: the `pp` query, without directives yet (#40).
add_library(ycxxc_pp STATIC)
target_sources(ycxxc_pp PUBLIC FILE_SET CXX_MODULES FILES pp.cppm)
target_link_libraries(ycxxc_pp PUBLIC ycxxc_lex)
