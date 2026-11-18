# Unit tests of ycxx's libraries, one executable per library, run by ctest (and so by tools/build
# and the build stage of tools/test).
add_executable(unit_ir ir/ir_test.cpp)
target_link_libraries(unit_ir PRIVATE ycxxc_ir)
add_test(NAME unit.ir COMMAND unit_ir)
