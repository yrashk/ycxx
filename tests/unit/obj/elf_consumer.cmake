# Only tests invoke the host toolchain to consume the ycxx ELF object as a black box.
execute_process(COMMAND "${WRITER}" elf-fixture.o COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${CC}" "${SOURCE}" elf-fixture.o -o elf-consumer COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND ./elf-consumer COMMAND_ERROR_IS_FATAL ANY)
