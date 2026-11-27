// #117 Itanium C++ ABI §5.1.6: a local class is a <local-name>, `Z <encoding> E <name>`, with a
// discriminator for a later class of the same name in its function and `Ut [n] _` for unnamed
// ones, or the typedef-name that names one; a function template specialization's local class is in
// its specialization's encoding. This unit defines functions whose parameters have these types and
// the peer calls them, so the peer's references must name ycxx's symbols ([class.local],
// [basic.link]). (The peer odr-uses them without defining them, which [basic.link] leaves without
// a required diagnostic for types without linkage; the test needs exactly that reference.)
// PEER: local-class-names-peer.cpp
// EXIT: 0
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

int take_first(decltype(first()) a) { return a.v; }
int take_second(decltype(second()) a) { return a.v; }
int take_unnamed(decltype(unnamed()) a) { return a.v; }
int take_template(decltype(in_template<int>()) a) { return a.v; }
int take_typedef(decltype(typedef_named()) a) { return a.v; }

int peer();

int main() { return peer(); }
