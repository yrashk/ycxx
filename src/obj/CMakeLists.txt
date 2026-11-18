# ycxx.obj: the in-memory object file; ycxx.obj.macho: Mach-O constants and the MH_OBJECT writer
# and reader (#42).
add_library(ycxxc_obj STATIC)
target_sources(ycxxc_obj PUBLIC FILE_SET CXX_MODULES FILES obj.cppm macho.cppm)
target_link_libraries(ycxxc_obj PUBLIC ycxxc_options)
