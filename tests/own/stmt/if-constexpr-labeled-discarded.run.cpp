// #337 (ι, review): a discarded `if constexpr` branch that is a labeled statement is skipped whole
// in an instantiation ([stmt.if]/2, [stmt.label]).
// EXIT: 0
template <int N> int f() {
  int r = 0;
  if constexpr (N == 0) L: { r += 1; r += 2; } else r += 10;
  return r;
}
int main() { return f<1>() == 10 && f<0>() == 3 ? 0 : 1; }
