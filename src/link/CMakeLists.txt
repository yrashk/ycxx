# ycxx.link: the linker, from objects to a signed arm64 macOS executable (#42).
add_library(ycxxc_link STATIC)
target_sources(ycxxc_link PUBLIC FILE_SET CXX_MODULES FILES link.cppm sha256.cppm)
target_link_libraries(ycxxc_link PUBLIC ycxxc_obj ycxxc_hash)
