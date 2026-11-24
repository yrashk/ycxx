// #1089: the last PIC option wins; -fpic defines __PIC__ and __pic__ as 1.
// FLAGS: --target=x86_64-linux-gnu -fPIC -fpic
// ORACLE: clang
// XFAIL: gcc GCC does not support --target
__PIC__ __pic__ __PIE__ __pie__
