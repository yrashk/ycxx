// #157 [dcl.fct.def.default], [temp.inst]: each specialization of a class template decides what its
// defaulted special members are, and defines only those that are used.
// EXIT: 0
int copies = 0, moves = 0, ends = 0;

struct Tracked {
  int v;
  Tracked() : v(0) {}
  Tracked(int x) : v(x) {}
  Tracked(const Tracked& o) : v(o.v) { ++copies; }
  Tracked(Tracked&& o) noexcept : v(o.v) { o.v = -1; ++moves; }
  Tracked& operator=(const Tracked& o) { v = o.v; copies += 10; return *this; }
  Tracked& operator=(Tracked&& o) noexcept { v = o.v; o.v = -1; moves += 10; return *this; }
  ~Tracked() { ++ends; }
};

struct MoveOnly {
  int v;
  MoveOnly(int x) : v(x) {}
  MoveOnly(MoveOnly&& o) : v(o.v) { o.v = 0; }
  MoveOnly& operator=(MoveOnly&& o) { v = o.v; o.v = 0; return *this; }
};

template <class T>
struct Box {
  T value;
  Box() = default;
  constexpr Box(T v, int) : value(static_cast<T&&>(v)) {}
  Box(const Box&) = default;
  Box(Box&&) = default;
  Box& operator=(const Box&) = default;
  Box& operator=(Box&&) = default;
  ~Box() = default;
};

template <class T>
struct Wrapper {
  T* p;
  constexpr Wrapper(T& t) noexcept : p(&t) {}
  constexpr Wrapper(const Wrapper&) noexcept = default;
  constexpr Wrapper& operator=(const Wrapper&) noexcept = default;
};

template <class T, class U>
struct Pair {
  T first;
  U second[2];
  Pair(int a, int b, int c) : first(a), second{b, c} {}
  Pair(const Pair&) = default;
  Pair(Pair&&) = default;
};

template <class T>
struct Derived : Box<T> {
  T extra;
  Derived(int a, int b) : extra(b) { this->value.v = a; }
  Derived(const Derived&) = default;
  Derived(Derived&&) = default;
  Derived& operator=(Derived&&) = default;
};

template <class T>
struct Filled {
  T value = T(7);
  int rest;
  Filled() = default;
  Filled(const Filled&) = default;
};

// Declared after the templates that hold it.
struct Late {
  int v;
  Late(int x) : v(x) {}
  Late(const Late& o) : v(o.v + 1) {}
};

constexpr int wrapped() {
  int a = 1, b = 2;
  Wrapper<int> x(a), y(b), z(x);
  z = y;
  *z.p = 5;
  return a * 10 + b;
}
static_assert(wrapped() == 15);

constexpr int boxed() {
  Box<int> a(3, 0);
  Box<int> b(a);
  Box<int> c(static_cast<Box<int>&&>(b));
  c = a;
  c.value += 1;
  a = static_cast<Box<int>&&>(c);
  return a.value;
}
static_assert(boxed() == 4);

int main() {
  {
    Box<Tracked> a(Tracked(1), 0);
    copies = moves = ends = 0;
    Box<Tracked> b(a);
    if (copies != 1 || moves != 0 || b.value.v != 1) return 1;
    Box<Tracked> c(static_cast<Box<Tracked>&&>(a));
    if (copies != 1 || moves != 1 || a.value.v != -1 || c.value.v != 1) return 2;
    c = b;
    if (copies != 11) return 3;
    c = static_cast<Box<Tracked>&&>(b);
    if (moves != 11 || b.value.v != -1) return 4;
  }
  if (ends != 3) return 5;

  Box<MoveOnly> m(MoveOnly(4), 0);
  Box<MoveOnly> n(static_cast<Box<MoveOnly>&&>(m));
  if (n.value.v != 4 || m.value.v != 0) return 6;
  m = static_cast<Box<MoveOnly>&&>(n);
  if (m.value.v != 4) return 7;

  copies = moves = ends = 0;
  {
    Pair<int, Tracked> p(1, 2, 3);
    Pair<int, Tracked> q(p);
    Pair<int, Tracked> r(static_cast<Pair<int, Tracked>&&>(q));
    if (copies != 2 || moves != 2 || r.first != 1 || r.second[1].v != 3 || q.second[0].v != -1) return 8;
  }
  if (ends != 6) return 9;

  copies = moves = 0;
  Derived<Tracked> d(1, 2);
  moves = 0;
  Derived<Tracked> e(d);
  Derived<Tracked> f(static_cast<Derived<Tracked>&&>(e));
  if (copies != 2 || moves != 2 || f.value.v != 1 || f.extra.v != 2 || e.extra.v != -1) return 10;
  f = static_cast<Derived<Tracked>&&>(d);
  if (moves != 22) return 11;

  Filled<int> g;
  Filled<int> h = Filled<int>();
  if (g.value != 7 || h.value != 7 || h.rest != 0) return 12;
  Filled<Late> i;
  Filled<Late> j(i);
  if (j.value.v != 8 && j.value.v != 9) return 13;
  Box<Late> k(Late(5), 0);
  const int before = k.value.v;
  Box<Late> l(k);
  return l.value.v == before + 1 ? 0 : 14;
}
