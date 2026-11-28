# ycxx.sema: semantic actions, diagnostics, constant evaluation, and the stand-in for #33 (#41).
add_library(ycxxc_sema ${YCXXC_COMPONENT_TYPE})
target_sources(ycxxc_sema PUBLIC FILE_SET CXX_MODULES FILES
  sema.cppm types.cppm diag.cppm templates.cppm actions.cppm eval.cppm)
target_sources(ycxxc_sema PRIVATE
  actions_lookup.cpp actions_calls.cpp actions_expr.cpp)
target_link_libraries(ycxxc_sema PUBLIC ycxxc_ast ycxxc_lex ycxxc_engine ycxxc_irgen ycxxc_interp)
