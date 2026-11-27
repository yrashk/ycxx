// A lambda in `decltype` in a function template's trailing return type (#1549,
// [expr.prim.lambda.general]/3, [temp.deduct]/7): each specialization's closure type is its own.
// XFAIL: ycxx a function template's signature with a dependent decltype stays Dependent (#1269)
template <class A, class B>
struct same {
  static constexpr bool value = false;
};
template <class A>
struct same<A, A> {
  static constexpr bool value = true;
};
template <class T>
auto f(T t) -> decltype([] { return T{}; }()) {
  return t + 1;
}
template <class T>
auto p(T& t) -> decltype([] { return static_cast<T*>(nullptr); }()) {
  return &t;
}
static_assert(same<decltype(f(1)), int>::value && same<decltype(f(1.0)), double>::value);
int main() {
  int i = 0;
  if (f(41) != 42 || f(1.5) != 2.5) return 1;
  return p(i) == &i ? 0 : 2;
}
