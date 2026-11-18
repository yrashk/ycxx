# The LSP front (docs/design/daemon.md §6, #99): JSON, framing and the language-server session,
# which the daemon runs per `ycxx lsp` connection.
add_library(ycxxc_lsp STATIC)
target_sources(ycxxc_lsp PUBLIC FILE_SET CXX_MODULES FILES json.cppm lsp.cppm)
target_link_libraries(ycxxc_lsp PUBLIC ycxxc_options)
