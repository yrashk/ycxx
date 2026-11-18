# The resident daemon (docs/design/daemon.md): the server, and what clients use to reach it. The
# :os and :watch partitions have one file per platform.
if(APPLE)
  set(YCXXC_DAEMON_OS os_darwin.cppm)
  set(YCXXC_DAEMON_WATCH watch_darwin.cppm)
else()
  set(YCXXC_DAEMON_OS os_linux.cppm)
  set(YCXXC_DAEMON_WATCH watch_linux.cppm)
endif()
add_library(ycxxc_daemon STATIC)
target_sources(ycxxc_daemon PUBLIC FILE_SET CXX_MODULES FILES
  ${YCXXC_DAEMON_OS}
  posix.cppm
  scope.cppm
  protocol.cppm
  connect.cppm
  events.cppm
  files.cppm
  admission.cppm
  ${YCXXC_DAEMON_WATCH}
  server.cppm
  daemon.cppm)
target_link_libraries(ycxxc_daemon PUBLIC ycxxc_compile ycxxc_engine ycxxc_pp)
if(APPLE)
  # FSEvents.h alone: CoreServices.h pulls in headers with blocks, which GCC cannot parse.
  set_source_files_properties(watch_darwin.cppm PROPERTIES COMPILE_OPTIONS
    "-F${CMAKE_OSX_SYSROOT}/System/Library/Frameworks/CoreServices.framework/Frameworks")
  target_link_libraries(ycxxc_daemon PUBLIC "-framework CoreServices")
endif()
