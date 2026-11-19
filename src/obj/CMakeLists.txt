# The in-memory object, its Mach-O reader and writer, and its ELF64 writer.
add_library(ycxxc_obj STATIC)
target_sources(ycxxc_obj PUBLIC FILE_SET CXX_MODULES FILES obj.cppm macho.cppm elf.cppm)
target_link_libraries(ycxxc_obj PUBLIC ycxxc_options)
