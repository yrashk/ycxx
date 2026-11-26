// Compiled by the peer compiler: the same inline functions, and calls to the test's functions.
inline auto first() { struct A { int v; }; return A{1}; }
inline auto second() {
  { struct A { int w; } inner{0}; (void)inner; }
  struct A { int v; };
  return A{2};
}
inline auto unnamed() {
  struct { int v; } a{3};
  struct { int v; } b{4};
  b.v += a.v - 3;
  return b;
}
inline auto typedef_named() {
  typedef struct {
    int v;
  } T;  // T names the class for linkage ([dcl.typedef]/9)
  return T{6};
}
template <class T> auto in_template() {
  struct B { T v; };
  return B{5};
}

int take_first(decltype(first()));
int take_second(decltype(second()));
int take_unnamed(decltype(unnamed()));
int take_template(decltype(in_template<int>()));
int take_typedef(decltype(typedef_named()));

int peer() {
  if (take_first(first()) != 1) return 1;
  if (take_second(second()) != 2) return 2;
  if (take_unnamed(unnamed()) != 4) return 3;
  if (take_template(in_template<int>()) != 5) return 4;
  if (take_typedef(typedef_named()) != 6) return 5;
  return 0;
}
