# The in-memory object and its Mach-O and ELF64 readers and writers.
add_library(ycxxc_obj STATIC)
target_sources(ycxxc_obj PUBLIC FILE_SET CXX_MODULES FILES obj.cppm macho.cppm elf.cppm)
target_link_libraries(ycxxc_obj PUBLIC ycxxc_options)
