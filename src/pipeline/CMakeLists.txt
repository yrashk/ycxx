# The pipeline of one translation unit (#43): front end, function_ir, lower, codegen and link.
add_library(ycxxc_pipeline STATIC)
target_sources(ycxxc_pipeline PUBLIC FILE_SET CXX_MODULES FILES
  lines.cppm
  mangle.cppm
  back.cppm
  executable.cppm
  pipeline.cppm)
target_link_libraries(ycxxc_pipeline PUBLIC ycxxc_irgen ycxxc_aarch64 ycxxc_link)
