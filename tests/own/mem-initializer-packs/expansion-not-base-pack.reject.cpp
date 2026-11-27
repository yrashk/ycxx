// #1400: a mem-initializer followed by `...` initializes the base classes that a pack expansion
// in the base-specifier-list specifies ([class.base.init], [temp.variadic]: the pattern is a
// mem-initializer whose mem-initializer-id denotes a base class). Neither a member nor a single
// base with a pack only in its arguments can be expanded so.
struct A { int a; A() : a(7) {} A(int x) : a(x) {} };
template<class B> struct D : B {
  template<class... T> D(T... t) : B(t)... {} //! error
};
struct E {
  A v;
  template<class... T> E(T... t) : v(t)... {} //! error
};
int main() { return 0; }
