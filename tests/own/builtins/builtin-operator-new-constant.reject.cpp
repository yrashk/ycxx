// [expr.const.core]: constant evaluation may allocate only through new-expressions and
// std::allocator<T>::allocate, so untyped storage from a direct __builtin_operator_new is not a
// constant expression. Clang agrees. ycxx rejects any constant evaluation that reaches the
// builtins until it has constexpr allocation (#321). See builtin-operator-new-constant-evaluation
// in docs/compatibility.toml.
// XFAIL: gcc GCC 16 accepts untyped __builtin_operator_new storage released before the evaluation ends
constexpr int untyped() {
  void* p = __builtin_operator_new(4);
  __builtin_operator_delete(p);
  return 1;
}
static_assert(untyped() == 1); //! error
