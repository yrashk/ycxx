// ζ c2 (#88): member functions defined in a class template use this, explicitly and
// implicitly, read and write data members of dependent type, call other member functions, and
// may use a member declared later in the class ([temp.class], [class.mfct.non.static],
// [expr.prim.this], [class.mem]).
// EXIT: 0
template<class T>
struct Counter {
  T count;
  void add(T n) { this->count += n; }
  void twice(T n) { add(n); this->add(n); }
  T get() const { return count + extra; }
  Counter* self() { return this; }
  T extra;
};

int main() {
  Counter<int> c{0, 0};
  c.add(3);
  if (c.get() != 3) return 1;
  c.twice(2);
  if (c.get() != 7) return 2;
  if (c.self() != &c) return 3;
  c.self()->add(1);
  if (c.count != 8) return 4;
  c.extra = 100;
  if (c.get() != 108) return 5;
  Counter<long> l{10L, 0L};
  l.twice(-5L);
  if (l.get() != 0L) return 6;
  return 0;
}
