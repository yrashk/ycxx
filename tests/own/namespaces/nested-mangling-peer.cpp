// The peer of nested-mangling.run.cpp, compiled by Clang.
namespace a::b { int f(int x); }
namespace v::inline w { int g(int x); }
namespace a::b::c { int peer_h(int x) { return a::b::f(x) + 3; } }
namespace v::inline w { int peer_k(int x) { return g(x) * 2; } }
