# Build-time policy checks (DECISIONS §9.2, stage 1). `cmake --build <dir> --target policy` runs
# them; CI runs them before the build.
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
add_custom_target(policy
  COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/tools/check-preprocessor
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
  COMMENT "Preprocessor policy (DECISIONS §3.1, deps/libycxx/DECISIONS.md §1)"
  VERBATIM)
