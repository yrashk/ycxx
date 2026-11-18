# ycxx.engine: the query engine's core (docs/design/query-engine.md, #14).
add_library(ycxxc_engine STATIC)
target_sources(ycxxc_engine PUBLIC FILE_SET CXX_MODULES FILES engine.cppm serial.cppm)
target_link_libraries(ycxxc_engine PUBLIC ycxxc_hash)
