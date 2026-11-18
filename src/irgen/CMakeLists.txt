# IR generation (#24): the analysed AST to L1.
add_library(ycxxc_irgen STATIC)
target_sources(ycxxc_irgen PUBLIC FILE_SET CXX_MODULES FILES
  facts.cppm
  gen.cppm
  types.cppm
  irgen.cppm)
target_link_libraries(ycxxc_irgen PUBLIC ycxxc_ast ycxxc_ir)
