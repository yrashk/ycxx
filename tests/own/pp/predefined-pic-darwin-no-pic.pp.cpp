// #1089: on arm64 Darwin, -fno-pic leaves __PIC__ and __pic__ at 2, as in Clang.
// FLAGS: --target=arm64-apple-darwin -fPIC -fno-pic
// ORACLE: clang
// XFAIL: gcc GCC does not support --target
__PIC__ __pic__ __PIE__ __pie__
