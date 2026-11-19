// #153, ι (#337): nested namespace definitions (C++17, [namespace.def]): `namespace a::b { }`
// declares `a` in the enclosing namespace and `b` in `a`, and reopens either if it exists; members
// are found qualified and, from inside, unqualified ([basic.lookup.unqual]).
// EXIT: 0
namespace a::b { int f() { return 1; } }
namespace a { int g() { return b::f() + 1; } }      // `a` exists after `namespace a::b`
namespace a::b::c { int h() { return f() + g() + 10; } }
namespace a::b { int f2() { return c::h(); } }      // reopened
namespace p { int q = 4; }
namespace p::r { int s() { return q + 1; } }         // `p` reopened, its members visible
int main() {
  if (a::b::f() != 1) return 1;
  if (a::g() != 2) return 2;
  if (a::b::c::h() != 13) return 3;
  if (a::b::f2() != 13) return 4;
  if (p::r::s() != 5) return 5;
  using namespace a::b::c;
  if (h() != 13) return 6;
  return 0;
}
