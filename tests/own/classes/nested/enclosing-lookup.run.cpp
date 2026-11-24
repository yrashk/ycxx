// #306 [class.nest]/1, [basic.lookup.unqual]/2: names in a nested class's members are looked up
// in the nested class, then in each enclosing class, then in the namespace.
struct O {
  static constexpr int base = 10;
  enum E { e1 = 1 };
  using T = int;
  struct I {
    T x;
    int get() const { return x + base + e1; }
    I next() const { return I{x + 1}; }
  };
  I make(int v) const { return I{v}; }
  int use() const { I i = make(5); return i.next().get(); }
};

int O_I_value(O::I i) { return i.x; }

int main() {
  O o;
  return o.use() - 17 + O_I_value(O::I{0});
}
