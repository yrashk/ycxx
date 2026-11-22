// The other side of enum-template-args.run.cpp, compiled by Clang: declarations only.
enum class K { a, b };
enum U : short { u0, u1 = -3 };
namespace n { enum class N : unsigned char { x = 200 }; }
template<K k> int f();
template<U u> int h();
template<n::N v> int m();
template<K k> struct S { static int g(); };

int peer() {
  if (f<K::a>() + f<K::b>() != 3 || h<u1>() != -3 || m<n::N::x>() != 200) return 11;
  if (S<K::b>::g() != 20) return 12;
  return 0;
}
