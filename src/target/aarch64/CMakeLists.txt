# ycxx.target.aarch64: the AArch64 target description, selection, allocation and emission (#28).
add_library(ycxxc_aarch64 STATIC)
target_sources(ycxxc_aarch64 PUBLIC FILE_SET CXX_MODULES FILES
  isa.cppm
  abi.cppm
  mir.cppm
  patterns.cppm
  select.cppm
  emit.cppm
  aarch64.cppm)
target_link_libraries(ycxxc_aarch64 PUBLIC ycxxc_lower ycxxc_obj)
