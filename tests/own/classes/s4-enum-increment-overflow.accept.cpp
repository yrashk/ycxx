// [dcl.enum]/5 and [conv.prom]/3: the available extended type holds the incremented value.
// FLAGS: -Wno-pedantic
// XFAIL: clang diagnoses the overflow of the 64-bit enumerator under -pedantic-errors
enum Y { c = 0xFFFFFFFFFFFFFFFFULL, d };
static_assert(sizeof(Y) == 16);
static_assert(d == (__int128(1) << 64));
