// #117 Itanium C++ ABI §5.1.6: a local class is a <local-name>, `Z <encoding> E <name>`, with a
// discriminator for a later class of the same name in its function and `Ut [n] _` for unnamed
// ones; a function template specialization's local class is in its specialization's encoding.
// The peer defines the functions whose parameters have these types ([class.local], [basic.link]).
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
template <class T> auto in_template() {
  struct B { T v; };
  return B{5};
}

int peer_first(decltype(first()));
int peer_second(decltype(second()));
int peer_unnamed(decltype(unnamed()));
int peer_template(decltype(in_template<int>()));

int main() {
  if (peer_first(first()) != 1) return 1;
  if (peer_second(second()) != 2) return 2;
  if (peer_unnamed(unnamed()) != 4) return 3;
  if (peer_template(in_template<int>()) != 5) return 4;
  return 0;
}
