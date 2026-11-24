// #1172: a 128-bit array bound beyond 64 bits is rejected ([dcl.array]), not read from its low half.
// FLAGS: -Wno-pedantic
int a[((__int128)1 << 64) + 2]; //! error
