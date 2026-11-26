# One binary, many fronts (DECISIONS §4.7): the daemon and its clients are modules of one
# executable. src/driver picks the front from the command line; src/daemon and src/client hold
# the fronts themselves.
# Libraries, lowest first.
add_subdirectory(execution)
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


add_library(ycxx::engine ALIAS ycxxc_engine)
set_target_properties(ycxxc_engine PROPERTIES EXPORT_NAME engine)
add_library(ycxx::preprocessor ALIAS ycxxc_pp)
set_target_properties(ycxxc_pp PROPERTIES EXPORT_NAME preprocessor)
add_library(ycxx::frontend ALIAS ycxxc_parse)
set_target_properties(ycxxc_parse PROPERTIES EXPORT_NAME frontend)
add_library(ycxx::ir ALIAS ycxxc_ir)
set_target_properties(ycxxc_ir PROPERTIES EXPORT_NAME ir)
add_library(ycxx::object ALIAS ycxxc_obj)
set_target_properties(ycxxc_obj PROPERTIES EXPORT_NAME object)
add_library(ycxx::linker ALIAS ycxxc_link)
set_target_properties(ycxxc_link PROPERTIES EXPORT_NAME linker)
add_library(ycxx::compiler ALIAS ycxxc_pipeline)
set_target_properties(ycxxc_pipeline PROPERTIES EXPORT_NAME compiler)
add_library(ycxx::project_build ALIAS ycxxc_project_build)
set_target_properties(ycxxc_project_build PROPERTIES EXPORT_NAME project_build)

configure_file(version.cppm.in ${CMAKE_CURRENT_BINARY_DIR}/version.cppm @ONLY)

add_library(ycxxc_lib ${YCXXC_COMPONENT_TYPE})
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
