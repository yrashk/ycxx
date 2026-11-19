// #229: a GNU built-in function ycxx does not have yet is one error where it is called, its
// arguments skipped, types among them (docs/EXTENSIONS.md); a declared `__builtin_` name (a
// function, a member, one a using-declaration brings in) is an ordinary name.
// XFAIL: clang,gcc they have these built-ins
struct S {
  int a, b;
};
unsigned long at() { return __builtin_offsetof(S, b); } //! error
int big() { return __builtin_constant_p(1) ? 1 : 0; } //! error
int unknown() { return __builtin_no_such_thing(1); } //! error
int __builtin_ours(int x) { return x; }
int ours() { return __builtin_ours(2); }
struct M {
  static int __builtin_member(int x) { return x; }
  int use() { return __builtin_member(3); }
};
namespace N {
int __builtin_named(int x) { return x; }
}  // namespace N
int named() {
  using N::__builtin_named;
  return __builtin_named(4);
}
