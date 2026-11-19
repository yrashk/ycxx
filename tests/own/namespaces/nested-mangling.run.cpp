// #153, ι (#337): functions of nested namespaces have the names Clang gives them (Itanium C++ ABI
// §5.1.5, an inline namespace being a component like any other), so ycxx and Clang objects call
// each other.
// PEER: nested-mangling-peer.cpp
// EXIT: 0
namespace a::b { int f(int x) { return x + 1; } }
namespace v::inline w { int g(int x) { return x * 2; } }
namespace a::b::c { int peer_h(int x); }
namespace v::inline w { int peer_k(int x); }
int main() {
  if (a::b::c::peer_h(3) != 7) return 1;  // the peer calls a::b::f back
  if (v::peer_k(5) != 20) return 2;       // and v::w::g
  return 0;
}
