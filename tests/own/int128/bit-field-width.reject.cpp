// #1172: a 128-bit bit-field width beyond 64 bits is rejected ([class.bit]), not read from its low
// half.
// FLAGS: -Wno-pedantic
// XFAIL: gcc GCC 16 accepts a width beyond the type's, as [class.bit]/1 allows, with padding bits
struct S {
  int x : ((__int128)1 << 64) + 3; //! error
};
