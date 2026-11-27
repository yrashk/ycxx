# ycxx.engine: the query engine (docs/design/query-engine.md): the core (#14) and the scheduler (#15).
add_library(ycxxc_engine ${YCXXC_COMPONENT_TYPE})
if(APPLE)
  set(YCXXC_ENGINE_STORAGE_OS storage_darwin.cppm)
else()
  set(YCXXC_ENGINE_STORAGE_OS storage_linux.cppm)
endif()
target_sources(ycxxc_engine PUBLIC FILE_SET CXX_MODULES
  FILES engine.cppm serial.cppm arena.cppm pool.cppm storage.cppm ${YCXXC_ENGINE_STORAGE_OS})
target_link_libraries(ycxxc_engine PUBLIC ycxxc_hash ycxxc_memory ycxxc_execution)
