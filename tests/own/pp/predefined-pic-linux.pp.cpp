// #1089: the last PIC option wins; -fPIC defines __PIC__ and __pic__ as 2.
// FLAGS: --target=x86_64-linux-gnu -fno-pic -fpic -fPIC
// ORACLE: clang
// XFAIL: gcc GCC does not support --target
__PIC__ __pic__ __PIE__ __pie__
