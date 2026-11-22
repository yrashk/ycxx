ycxxc_unit_test(elf elf_test.cpp ycxxc_obj ycxxc_hash)
ycxxc_unit_test(macho_obj macho_test.cpp ycxxc_obj)
# PARITY: by design: links its ELF output with the host's C compiler and runs it
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$"
   AND NOT CMAKE_CROSSCOMPILING)
  add_test(NAME unit.elf_consumer
    COMMAND ${CMAKE_COMMAND} -DWRITER=$<TARGET_FILE:unit_elf> -DCC=${CMAKE_C_COMPILER}
            -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/elf_consumer.c
            -P ${CMAKE_CURRENT_SOURCE_DIR}/elf_consumer.cmake)
endif()
