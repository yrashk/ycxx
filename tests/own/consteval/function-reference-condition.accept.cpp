// #102, [expr.const.defns], [expr.const.imm]: the constexpr condition variable's initializer
// is manifestly constant-evaluated.
// XFAIL: clang Clang 23 rejects the address of the immediate function in this initializer.
consteval int immediate(int x) { return x + 1; }
constexpr int call_with_pointer(int (*function)(int), int x) { return function(x); }

int condition() {
  if (constexpr int n = call_with_pointer(immediate, 41))
    return n;
  return 0;
}
