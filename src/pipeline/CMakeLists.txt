# The pipeline (#43, #63): front end, function_ir, lower, codegen, tu and link, as engine queries.
add_library(ycxxc_pipeline STATIC)
target_sources(ycxxc_pipeline PUBLIC FILE_SET CXX_MODULES FILES
  lines.cppm
  mangle.cppm
  back.cppm
  front.cppm
  executable.cppm
  session.cppm
  pipeline.cppm)
target_link_libraries(ycxxc_pipeline PUBLIC ycxxc_parse ycxxc_irgen ycxxc_aarch64 ycxxc_link)
