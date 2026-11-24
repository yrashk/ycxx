// #1089: the last PIC option wins; -fno-pic defines neither PIC nor PIE macros.
// FLAGS: --target=x86_64-linux-gnu -fPIC -fno-pic
// ORACLE: clang
// XFAIL: gcc GCC does not support --target
__PIC__ __pic__ __PIE__ __pie__
