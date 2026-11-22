// #102, [expr.const.imm], [temp.arg.nontype]: typed function-valued constant template arguments.
// XFAIL: clang Clang 23 rejects immediate functions as constant template arguments.
consteval int immediate(int x) { return x + 1; }

template <int (*Function)(int)> struct PointerArgument {};

PointerArgument<immediate> decay;
PointerArgument<&immediate> address;
