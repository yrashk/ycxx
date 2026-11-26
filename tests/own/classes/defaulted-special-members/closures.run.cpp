// #157, #338 [temp.inst], [dcl.fct.def.default]: defaulted members that only a closure uses, of a
// variable template's specialization or of a class template's static member, are defined.
// EXIT: 0
int ends = 0;
struct N {
  int v;
  N(int x = 1) : v(x) {}
  N(const N& o) : v(o.v + 1) {}
  N& operator=(const N& o) { v = o.v + 10; return *this; }
  ~N() { ++ends; }
};
struct A { N n; A() = default; A(const A&) = default; A& operator=(const A&) = default; ~A() = default; };
struct B { A a; B() = default; B(const B&) = default; B& operator=(const B&) = default; ~B() = default; };
struct V { N n; V(const V&) = default; V(int) {} virtual int g() { return 4; } virtual ~V() = default; };
struct W : V { W() : V(1) {} W(const W&) = default; int g() override { return 5; } };

template <class T> constexpr auto made = [] { T c; return c.a.n.v; };
template <class T> constexpr auto copied = [](const T& a) { T b(a); return b.a.n.v; };
template <class T> constexpr auto called = [](const T& a) { T b(a); return b.g(); };
template <class T> struct S {
  static constexpr auto copy = [](const T& a) { T b(a); return b.a.n.v; };
  static constexpr auto assign = [](T& c, const T& a) { c = a; return c.a.n.v; };
};

int main() {
  if (made<B>() != 1) return 1;
  B b;
  if (copied<B>(b) != 2 || S<B>::copy(b) != 2) return 2;
  B c;
  if (S<B>::assign(c, b) != 11) return 3;
  W w;
  if (called<W>(w) != 5) return 4;
  return ends == 4 ? 0 : 5;
}
