// ι (#300): a `const T&` parameter defaulted to a temporary. The temporary is created for each
// call that uses the default, is alive during the call, and is destroyed at the end of the
// full-expression containing the call, not when the call returns ([dcl.fct.default]/1,
// [class.temporary]/7 (a reference parameter), [expr.call]/5).
// EXIT: 0
int made = 0;
int destroyed = 0;

struct S {
  int v;
  S(int v) : v(v) { ++made; }
  ~S() { ++destroyed; }
};

int use(const S& s = S(3)) { return s.v * 10 + destroyed; }
int after(int r) { return r * 10 + destroyed; }  // still in the full-expression of use()

int main() {
  int r = use();
  if (r != 30) return 1;          // alive during the call
  if (made != 1 || destroyed != 1) return 2;
  r = after(use());
  if (r != 311) return 3;         // use() sees 1 earlier destruction, after() still sees 1
  if (made != 2 || destroyed != 2) return 4;
  S mine(8);
  if (use(mine) != 82) return 5;  // a supplied argument: no temporary
  if (made != 3 || destroyed != 2) return 6;
  return 0;
}
