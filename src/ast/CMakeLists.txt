# The AST core (docs/design/ast.md, #32): handles, interners, the reflection-derived node schema,
# frame builders and the red view.
add_library(ycxxc_ast ${YCXXC_COMPONENT_TYPE})
target_sources(ycxxc_ast PUBLIC FILE_SET CXX_MODULES FILES
  fingerprint.cppm
  handles.cppm
  store.cppm
  schema.cppm
  nodes.cppm
  records.cppm
  types.cppm
  values.cppm
  target.cppm
  interner.cppm
  builder.cppm
  red.cppm
  compact.cppm
  typeops.cppm
  ast.cppm)
target_link_libraries(ycxxc_ast PUBLIC ycxxc_hash ycxxc_options)
# BOOTSTRAP: contracts: at -O1 and above GCC 16.2 inlines `KindTable<…>::kind`'s precondition
# (a specialization instantiated in ycxx.ast.interner) into `Compactor`, but does not emit the
# TU-local violation helper (`__tu_has_violation`) it calls, so every program linking the AST
# fails to link (#46; GCC PR 124264, of which this is the template case). At -O0 the unit's own
# contracts happen to emit the helper. -fkeep-static-functions keeps it; without the bug this
# line goes.
set_source_files_properties(compact.cppm PROPERTIES COMPILE_OPTIONS -fkeep-static-functions)
