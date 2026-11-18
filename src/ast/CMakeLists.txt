# The AST core (docs/design/ast.md, #32): handles, interners, the reflection-derived node schema,
# frame builders and the red view.
add_library(ycxxc_ast STATIC)
target_sources(ycxxc_ast PUBLIC FILE_SET CXX_MODULES FILES
  fingerprint.cppm
  handles.cppm
  schema.cppm
  nodes.cppm
  records.cppm
  interner.cppm
  builder.cppm
  red.cppm
  compact.cppm
  ast.cppm)
target_link_libraries(ycxxc_ast PUBLIC ycxxc_options)
