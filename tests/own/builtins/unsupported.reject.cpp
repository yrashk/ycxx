// #229: a GNU built-in function ycxx does not have yet is one error where it is called, its
// arguments skipped, types among them (docs/EXTENSIONS.md); a declared `__builtin_` name is an
// ordinary name.
// XFAIL: clang,gcc they have these built-ins
struct S {
  int a, b;
};
unsigned long at() { return __builtin_offsetof(S, b); } //! error
int big() { return __builtin_constant_p(1) ? 1 : 0; } //! error
int __builtin_ours(int x) { return x; }
int ours() { return __builtin_ours(2); }
