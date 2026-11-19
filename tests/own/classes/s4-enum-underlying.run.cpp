// #220: the underlying type of an unscoped enumeration without a fixed type ([dcl.enum]/7) is
// the one the Itanium C++ ABI compilers choose, at every boundary: with no negative enumerator,
// unsigned int if every value fits, else unsigned long; with one, int if every value fits, else
// long. An empty enumeration's is unsigned int. Its size follows ([expr.sizeof]).
// EXIT: 0
enum A { a0, a1 };
enum B { b0 = 0x7fffffff };
enum C { c0 = 0x80000000u };
enum D { d0 = 0xffffffffu };
enum E { e0 = 0x100000000LL };
enum F { f0 = -1 };
enum G { g0 = -1, g1 = 0x7fffffff };
enum H { h0 = -1, h1 = 0x80000000LL };
enum I { i0 = 0x7fffffffffffffffLL };
enum J { j0 = 0x8000000000000000ULL };
enum K { k0 = -0x7fffffffffffffffLL - 1 };
enum L {};
enum N { n0 = 0xffffffffu, n1 };           // n1 is 2^32: unsigned long
static_assert(__is_same(__underlying_type(A), unsigned int));
static_assert(__is_same(__underlying_type(B), unsigned int));
static_assert(__is_same(__underlying_type(C), unsigned int));
static_assert(__is_same(__underlying_type(D), unsigned int));
static_assert(__is_same(__underlying_type(E), unsigned long));
static_assert(__is_same(__underlying_type(F), int));
static_assert(__is_same(__underlying_type(G), int));
static_assert(__is_same(__underlying_type(H), long));
static_assert(__is_same(__underlying_type(I), unsigned long));
static_assert(__is_same(__underlying_type(J), unsigned long));
static_assert(__is_same(__underlying_type(K), long));
static_assert(__is_same(__underlying_type(L), unsigned int));
static_assert(__is_same(__underlying_type(N), unsigned long));
static_assert(sizeof(A) == 4 && sizeof(E) == 8 && sizeof(H) == 8 && sizeof(L) == 4);
// The choice is visible at run time: the underlying type's arithmetic.
int below_zero(__underlying_type(A) u) { return u - 1 < 0; }
int main() { return below_zero(0); }  // unsigned: 0 - 1 wraps, not below zero
