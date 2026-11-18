# L1 IR (#23): the op table, bodies, canonical order, fingerprint, debug info, text form.
add_library(ycxxc_ir STATIC)
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
target_link_libraries(ycxxc_ir PUBLIC ycxxc_options)
