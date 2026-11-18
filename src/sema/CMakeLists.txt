# ycxx.sema: semantic actions, diagnostics, constant folding, and the stand-in for #33 (#41).
add_library(ycxxc_sema STATIC)
target_sources(ycxxc_sema PUBLIC FILE_SET CXX_MODULES FILES
  sema.cppm types.cppm diag.cppm actions.cppm fold.cppm)
target_link_libraries(ycxxc_sema PUBLIC ycxxc_ast ycxxc_lex ycxxc_engine)
