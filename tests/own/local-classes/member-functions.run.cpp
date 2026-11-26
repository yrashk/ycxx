// Local classes (#117), spec-only: a local class's member functions are defined in the class
// definition: non-static, const and static member functions, calling each other
// ([class.local], [class.mfct], [class.static.mfct], [class.this]).
int main() {
  struct Counter {
    int n;
    void bump() { n = add(n, 1); }
    void bump_twice() {
      bump();
      this->bump();
    }
    int get() const { return n; }
    int doubled() const { return get() * 2; }
    static int add(int a, int b) { return a + b; }
  };
  Counter c{5};
  c.bump();
  if (c.get() != 6) return 1;
  c.bump_twice();
  if (c.get() != 8) return 2;
  const Counter& cr = c;
  if (cr.doubled() != 16) return 3;
  if (Counter::add(2, 3) != 5) return 4;
  if (c.add(4, 4) != 8) return 5;
  return 0;
}
