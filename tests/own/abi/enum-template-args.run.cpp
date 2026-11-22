// #774: template arguments of enumeration type in symbols, `L <type> <value> E` with `n` before a
// negative value (Itanium C++ ABI §5.1.5.7). The Clang peer only declares the templates, so its
// calls link against the specializations this object defines.
// PEER: enum-template-args-peer.cpp
// EXIT: 0
enum class K { a, b };
enum U : short { u0, u1 = -3 };
namespace n { enum class N : unsigned char { x = 200 }; }
template<K k> int f() { return k == K::b ? 2 : 1; }
template<U u> int h() { return u; }
template<n::N v> int m() { return int(v); }
template<K k> struct S { static int g() { return f<k>() * 10; } };

int peer();

int main() {
  if (f<K::a>() + f<K::b>() != 3 || h<u1>() != -3 || m<n::N::x>() != 200) return 1;
  if (S<K::b>::g() != 20) return 2;
  return peer();
}
