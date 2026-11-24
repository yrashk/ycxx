// #1176: enumerations with a 128-bit underlying type or enumerator values ([dcl.enum]). ycxx keeps
// enumerator values in 64 bits and diagnoses these as not supported yet (#1172).
// FLAGS: -Wno-pedantic
// XFAIL: ycxx #1176: enumerator values are kept in 64 bits
enum E : __int128 { a = (__int128)1 << 100, b = -a };
static_assert(b < 0 && (__int128)a == (__int128)1 << 100);
