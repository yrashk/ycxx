# ycxx.link: signed arm64 macOS (#42) and static x86-64 Linux executable images.
add_library(ycxxc_link STATIC)
target_sources(ycxxc_link PUBLIC FILE_SET CXX_MODULES FILES link.cppm sha256.cppm elf.cppm)
target_link_libraries(ycxxc_link PUBLIC ycxxc_obj ycxxc_hash)
