// θ step 1 (#279): the Itanium names of function template specializations whose declared return
// type is a placeholder keep it as `Da` (auto) and `Dc` (decltype(auto)) (Itanium C++ ABI
// §5.1.5.2); the peer, compiled by Clang, calls them by those names.
// PEER: p1-impl-mangle-peer.cpp
template<class T> auto twice(T x) { return x + x; }
template<class T> decltype(auto) first(T& x) { return (x); }
template<class T> const auto& id(const T& x) { return x; }
int peer_check();
int main() {
  int v = 4;
  if (twice(3) != 6 || twice(2L) != 4 || first(v) != 4 || id(5) != 5) return 1;
  return peer_check();
}
