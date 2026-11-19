// #153, ι (#337): `namespace a::inline b` (C++20, [namespace.def]/2, /7): only `b` is inline, so
// its members are members of `a` for lookup; `a` itself is not.
// EXIT: 0
namespace x::inline y { int k() { return 100; } }
namespace x { int m() { return k(); } }
namespace x::inline y::z { int n() { return 7; } }
int main() {
  if (x::y::k() != 100) return 1;
  if (x::k() != 100) return 2;
  if (x::m() != 100) return 3;
  if (x::z::n() != 7) return 4;
  if (x::y::z::n() != 7) return 5;
  return 0;
}
