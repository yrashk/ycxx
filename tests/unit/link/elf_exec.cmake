set(image "${CMAKE_CURRENT_BINARY_DIR}/elf-linked")
execute_process(COMMAND "${LINKER}" "${image}" COMMAND_ERROR_IS_FATAL ANY)
file(CHMOD "${image}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
if(READELF)
  execute_process(COMMAND "${CMAKE_COMMAND}" -E env LC_ALL=C "${READELF}" -h -l -W "${image}"
    OUTPUT_VARIABLE headers COMMAND_ERROR_IS_FATAL ANY)
  if(NOT headers MATCHES "Type:[ \t]+EXEC" OR NOT headers MATCHES "LOAD[^\n]*R[ \t]*E[ \t]+0x1000"
     OR NOT headers MATCHES "LOAD[^\n]*R[ \t]+0x1000" OR NOT headers MATCHES "LOAD[^\n]*R[ \t]*W[ \t]+0x1000"
     OR NOT headers MATCHES "GNU_STACK[^\n]*R[ \t]*W[ \t]+0x10"
     OR headers MATCHES "INTERP|DYNAMIC")
    message(FATAL_ERROR "Unexpected ELF executable headers\n${headers}")
  endif()
endif()
execute_process(COMMAND "${image}" RESULT_VARIABLE status TIMEOUT 5)
if(NOT status STREQUAL "42")
  message(FATAL_ERROR "Expected the linked executable to exit 42, got ${status}")
endif()
