// δ s2 (#86): a member function body is a complete-class context: it uses data members and
// member functions declared later in the class, as do default member initializers
// ([class.mem.general] complete-class context, [basic.scope.class]).
// EXIT: 0
struct Timer {
  int elapsed() const { return now - start; }
  void tick() { advance(1); }
  void reset() { start = now; }
  int doubled() const { return twice(elapsed()); }

  int start = 0;
  int now = 0;

  void advance(int n) { now += n; }
  static int twice(int v) { return 2 * v; }
};

int main() {
  Timer t;
  t.tick();
  t.tick();
  t.advance(5);
  if (t.elapsed() != 7) return 1;
  if (t.doubled() != 14) return 2;
  t.reset();
  if (t.elapsed() != 0) return 3;
  t.tick();
  if (t.elapsed() != 1 || t.now != 8) return 4;
  return 0;
}
