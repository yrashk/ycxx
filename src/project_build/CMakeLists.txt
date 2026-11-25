add_library(ycxxc_build_manifest STATIC)
target_sources(ycxxc_build_manifest PUBLIC FILE_SET CXX_MODULES FILES manifest.cppm)
target_link_libraries(ycxxc_build_manifest PUBLIC ycxxc_options)

add_subdirectory(action)

add_library(ycxxc_project_build STATIC)
target_sources(ycxxc_project_build PUBLIC FILE_SET CXX_MODULES FILES
  rule_compilers.cppm
  rule_protocol.cppm
  rule_snapshot.cppm
  rule_worker.cppm
  toolchain_probe.cppm
  toolchain_adapter.cppm
  toolchain_inputs.cppm
  toolchain_matrix.cppm
  inspection_model.cppm
  inspection_html.cppm
  inspection.cppm
  plan.cppm
  project.cppm)
target_link_libraries(ycxxc_project_build PUBLIC ycxxc_build_manifest ycxxc_build_action ycxxc_pipeline ycxxc_lsp ${CMAKE_DL_LIBS})
