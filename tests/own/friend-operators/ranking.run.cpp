// #1178, from the review: [over.match.best], [over.match.oper], [temp.dep.candidate].
// EXIT: 0
// A reversed candidate is ranked with the direct one when the operands are dependent expressions.
namespace three_way {
struct A { int v; int operator<=>(long) const { return 1; } };
int operator<=>(int, const A&) { return 1; }
struct F {
  int v;
  friend int operator<=>(const F&, long) { return 1; }
  friend int operator<=>(int, F&) { return 1; }
};
template <class T, class U> bool less(T* a, U* b) { return *a < *b; }
}

// A ref-qualified member against non-members that the base's namespace adds.
namespace lib {
struct Base {};
Base& operator+=(Base& b, int) { return b; }
Base& operator++(Base& b) { return b; }
bool operator!(const Base&) { return false; }
int operator-(Base) { return 0; }
}
struct V : lib::Base {
  int n = 0;
  V& operator+=(int k) & { n += k; return *this; }
  V& operator++() & { ++n; return *this; }
  bool operator!() const& { return true; }
  int operator-() && { return 5; }
};

// A non-template member before a friend template with the same conversion.
struct M {
  template <class T> friend bool operator!(const T&) { return false; }
  bool operator!() const { return true; }
};
// A friend template where the member is not viable.
struct N {
  int operator~() && { return 1; }
  template <class T> friend int operator~(T&) { return 2; }
};

int main() {
  three_way::A a{1};
  three_way::F f{1};
  int i = 1;
  if (!three_way::less(&a, &i) || !three_way::less(&f, &i)) return 1;
  V v;
  v += 2;
  ++v;
  if (v.n != 3 || !v != true || -V{} != 5) return 2;
  if (!M{} != true) return 3;
  N n;
  return ~n == 2 ? 0 : 4;
}
