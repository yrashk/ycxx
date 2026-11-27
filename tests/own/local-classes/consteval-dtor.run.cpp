// Local classes (#117), spec-only: in a consteval function, a local class's constexpr destructor runs
// during constant evaluation at block exit, and its effect is observed ([class.dtor],
// [expr.const], [dcl.constexpr], [class.local]).
consteval int run() {
  int log = 0;
  {
    struct Mark {
      int& out;
      int id;
      constexpr ~Mark() { out = out * 10 + id; }
    };
    Mark a{log, 1};
    Mark b{log, 2};
  }
  return log;
}

static_assert(run() == 21);

int main() {
  constexpr int v = run();
  if (v != 21) return 1;
  return 0;
}
