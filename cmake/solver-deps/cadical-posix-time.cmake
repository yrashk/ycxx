# CaDiCaL calls clock_gettime from resources.cpp. Some standard libraries expose its
# declaration transitively. Include the owning POSIX header for libycxx as well.
set(resource_file "${SOURCE_DIR}/src/resources.cpp")
file(READ "${resource_file}" contents)
set(header "#include <sys/time.h>\n")
set(replacement "${header}#include <time.h>\n")
if(contents MATCHES "#include <time[.]h>")
  return()
endif()
if(NOT contents MATCHES "#include <sys/time[.]h>")
  message(FATAL_ERROR "CaDiCaL resources.cpp changed. Recheck its POSIX clock includes.")
endif()
string(REPLACE "${header}" "${replacement}" patched "${contents}")
file(WRITE "${resource_file}" "${patched}")
