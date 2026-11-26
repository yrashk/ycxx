# One binary, many fronts (DECISIONS §4.7): the daemon and its clients are modules of one
# executable. src/driver picks the front from the command line; src/daemon and src/client hold
# the fronts themselves.
# Libraries, lowest first.
add_subdirectory(memory)
add_subdirectory(hash)
add_subdirectory(cm)
add_subdirectory(stack)
add_subdirectory(ast)
add_subdirectory(engine)
add_subdirectory(trace)
add_subdirectory(lex)
add_subdirectory(pp)
add_subdirectory(ir)
add_subdirectory(items)
add_subdirectory(irgen)
add_subdirectory(interp)
add_subdirectory(obj)
add_subdirectory(link)
add_subdirectory(sema)
add_subdirectory(parse)
add_subdirectory(target/aarch64)
add_subdirectory(target/x86_64)
add_subdirectory(pipeline)
add_subdirectory(project_build)
add_subdirectory(lsp)
add_subdirectory(client)
add_subdirectory(daemon)

configure_file(version.cppm.in ${CMAKE_CURRENT_BINARY_DIR}/version.cppm @ONLY)

add_library(ycxxc_lib STATIC)
target_sources(ycxxc_lib PUBLIC FILE_SET CXX_MODULES
  BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}
  FILES
    ${CMAKE_CURRENT_BINARY_DIR}/version.cppm
    driver/driver.cppm
    driver/archive.cppm
    client/client.cppm)
target_link_libraries(ycxxc_lib PUBLIC ycxxc_options ycxxc_daemon ycxxc_obj)

add_executable(ycxxc main.cpp)
set_target_properties(ycxxc PROPERTIES OUTPUT_NAME ycxx RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
target_link_libraries(ycxxc PRIVATE ycxxc_lib ycxxc_new)
