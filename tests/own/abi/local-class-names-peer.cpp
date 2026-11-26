// Compiled by the peer compiler: the same inline functions, and the functions the test calls.
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
template <class T> auto in_template() {
  struct B { T v; };
  return B{5};
}

int peer_first(decltype(first()) a) { return a.v; }
int peer_second(decltype(second()) a) { return a.v; }
int peer_unnamed(decltype(unnamed()) a) { return a.v; }
int peer_template(decltype(in_template<int>()) a) { return a.v; }
