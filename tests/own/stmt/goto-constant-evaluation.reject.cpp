// #337 (ι, review): a goto statement is never evaluated in a constant expression ([expr.const]).
// Each case is on one line: Clang reports the use, GCC the goto.
constexpr int h(int x) { int n = 0; loop: if (n < x) { ++n; goto loop; } return n; } static_assert(h(5) == 5); //! error
constexpr int k(int x) { if (x) goto out; return 1; out: return 2; }
constexpr int b = k(0);  // the goto is not evaluated
constexpr int k2(int x) { if (x) goto out; return 1; out: return 2; } constexpr int c = k2(1); //! error
int main() { return h(1) + b + c; }
