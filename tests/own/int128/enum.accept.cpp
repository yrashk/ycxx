// #1176: enumerations with a 128-bit underlying type or enumerator values ([dcl.enum]).
// FLAGS: -Wno-pedantic
enum E : __int128 { a = (__int128)1 << 100, b = -a };
static_assert(b < 0 && (__int128)a == (__int128)1 << 100);
// An unfixed enumeration with 128-bit initializers whose values fit 64 bits.
enum G { g = (__int128)1 << 63 };
static_assert(g > 0 && sizeof(G) == 8);
enum H { h = (__int128)-5, i = h < 0 ? 1 : 2 };
static_assert(i == 1);
