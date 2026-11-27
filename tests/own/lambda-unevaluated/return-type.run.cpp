// A lambda in `decltype` in a function's declarator (#1549, [expr.prim.lambda.general]/3): a
// trailing return type, a parameter's type, in a member function and in a class template's.
// A function template's is function-template-return.run.cpp.
template <class A, class B>
struct same {
  static constexpr bool value = false;
};
template <class A>
struct same<A, A> {
  static constexpr bool value = true;
};
auto g() -> decltype([] { return 2L; }()) { return 5; }
int h(decltype([] { return 'a'; }()) c) { return c; }
struct S {
  auto m() -> decltype([] { return 'a'; }()) { return 'b'; }
  static auto s(decltype([] { return 1u; }()) u) { return u + 1; }
};
template <class T>
struct C {
  auto get() -> decltype([] { return static_cast<T*>(nullptr); }()) { return nullptr; }
};
static_assert(same<decltype(g()), long>::value && same<decltype(S{}.m()), char>::value);
static_assert(same<decltype(C<short>{}.get()), short*>::value);
int main() {
  if (g() != 5 || h('c') != 'c' || S{}.m() != 'b' || S::s(2u) != 3u) return 1;
  return C<int>{}.get() ? 3 : 0;
}
