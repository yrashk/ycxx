// ι (#300): a default argument of a member function is a complete-class context: it can name a
// static data member, an enumerator and a static member function declared after the function
// in the class ([class.mem.general] (complete-class context), [dcl.fct.default]/1, /9).
// EXIT: 0
struct Late {
  int add(int n = step) { return base_value() + n; }
  int mode(int m = fast) { return m; }
  static int twice(int x = later()) { return 2 * x; }

  static int step;
  enum Mode { slow = 1, fast = 2 };
  static int later() { return 21; }
  int base_value() { return 100; }
};

int Late::step = 5;

int main() {
  Late l;
  if (l.add() != 105) return 1;
  Late::step = 6;
  if (l.add() != 106) return 2;
  if (l.mode() != 2) return 3;
  if (Late::twice() != 42) return 4;
  if (l.twice(1) != 2) return 5;
  return 0;
}
