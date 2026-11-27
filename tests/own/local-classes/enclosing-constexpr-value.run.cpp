// Local classes (#117), spec-only: a local class may use the value of an enclosing constexpr
// automatic variable: the lvalue-to-rvalue conversion of a variable usable in constant
// expressions is not an odr-use ([basic.def.odr], [class.local], [expr.const]).
int main() {
  constexpr int n = 4;
  const int k = 6;
  struct S {
    int arr[n];
    int scaled(int x) const { return x * n + k; }
    static int size() { return n; }
  };
  S s{};
  if (sizeof(s.arr) != 4 * sizeof(int)) return 1;
  if (s.scaled(2) != 14) return 2;
  if (S::size() != 4) return 3;
  return 0;
}
