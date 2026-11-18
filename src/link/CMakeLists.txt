# ycxx.link: the linker, from objects to a signed arm64 macOS executable (#42).
add_library(ycxxc_link STATIC)
target_sources(ycxxc_link PUBLIC FILE_SET CXX_MODULES FILES link.cppm sha256.cppm)
target_link_libraries(ycxxc_link PUBLIC ycxxc_obj ycxxc_hash)

# ycxx-ld: links Mach-O objects from the command line, for tests (not a ycxx front).
add_executable(ycxxc_ld ld_main.cpp)
set_target_properties(ycxxc_ld PROPERTIES OUTPUT_NAME ycxx-ld RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
target_link_libraries(ycxxc_ld PRIVATE ycxxc_link)
