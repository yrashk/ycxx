// #133, from the step's review: an entity takes one asm label (docs/EXTENSIONS.md).
// XFAIL: gcc GCC takes the last of two different asm labels; Clang rejects them, as ycxx does
int f(int) __asm("_a");
int f(int) __asm("_b"); //! error
