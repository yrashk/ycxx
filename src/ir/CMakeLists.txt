# L1 IR (#23): the op table, bodies, canonical order, fingerprint, debug info, text form.
add_library(ycxxc_ir ${YCXXC_COMPONENT_TYPE})
target_sources(ycxxc_ir PUBLIC FILE_SET CXX_MODULES FILES
  fingerprint.cppm
  handles.cppm
  ops.cppm
  function.cppm
  builder.cppm
  identity.cppm
  debug.cppm
  text.cppm
  ir.cppm)
target_link_libraries(ycxxc_ir PUBLIC ycxxc_ast ycxxc_hash)

# L2, `lower` and `legalize` (#26, α subset; #1172).
add_library(ycxxc_lower ${YCXXC_COMPONENT_TYPE})
target_sources(ycxxc_lower PUBLIC FILE_SET CXX_MODULES FILES l2.cppm legalize.cppm lower.cppm)
target_link_libraries(ycxxc_lower PUBLIC ycxxc_ir)
