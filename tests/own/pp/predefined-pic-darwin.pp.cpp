// #1089: arm64 Darwin code is always PIC, so __PIC__ and __pic__ are 2 as in Clang.
// FLAGS: --target=arm64-apple-darwin
// ORACLE: clang
// XFAIL: gcc GCC does not support --target
__PIC__ __pic__ __PIE__ __pie__
