# ycxx.engine: the query engine (docs/design/query-engine.md): the core (#14) and the scheduler (#15).
add_library(ycxxc_engine STATIC)
target_sources(ycxxc_engine PUBLIC FILE_SET CXX_MODULES FILES engine.cppm serial.cppm arena.cppm pool.cppm)
target_link_libraries(ycxxc_engine PUBLIC ycxxc_hash ycxxc_memory)
