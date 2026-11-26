# The in-memory object, its Mach-O and ELF64 readers and writers, and archive containers.
add_library(ycxxc_obj ${YCXXC_COMPONENT_TYPE})
target_sources(ycxxc_obj PUBLIC FILE_SET CXX_MODULES FILES obj.cppm macho.cppm elf.cppm archive.cppm)
target_link_libraries(ycxxc_obj PUBLIC ycxxc_options ycxxc_hash ycxxc_engine)
