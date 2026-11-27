// #1400: mem-initializers whose targets depend on a template parameter ([temp.dep.type],
// [class.base.init]): a base that is a template parameter, `B(x)`, and a member of dependent
// type initialized with two arguments, which is checked only once the type is known.
// EXIT: 0
struct A { int a; A(int x) : a(x) {} };
struct P { int x, y; P(int a, int b) : x(a), y(b) {} };
template<class B> struct D : B { int m; D(int x) : B(x), m(x + 1) {} };
template<class T> struct M { T value; M() : value(1, 2) {} };
template<class B> struct F : B { template<class... T> F(T... t) : B(t...) {} };
int main() {
  D<A> d(3);
  if (d.a != 3 || d.m != 4) return 1;
  M<P> m;
  if (m.value.x != 1 || m.value.y != 2) return 2;
  F<P> f(5, 6);
  if (f.x != 5 || f.y != 6) return 3;
  return 0;
}
