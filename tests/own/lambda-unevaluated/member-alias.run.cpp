// A lambda in `decltype` in a member alias of a class template (#1549): an unevaluated operand
// ([expr.prim.lambda.general]/3), whose closure type each specialization has its own of
// ([expr.prim.lambda.closure]/1, [temp.inst]/3).
template <class A, class B>
struct same {
  static constexpr bool value = false;
};
template <class A>
struct same<A, A> {
  static constexpr bool value = true;
};
template <class I>
struct T {
  using pointer = decltype([] { return static_cast<I*>(nullptr); }());
  using closure = decltype([] {});
};
// As `iterator_traits::pointer` in libycxx: `if constexpr` on a requires-expression in the body.
struct HasPointer {
  using pointer = long*;
};
struct NoPointer {};
template <class I>
struct traits {
  using pointer = decltype([] {
    if constexpr (requires { typename I::pointer; }) return static_cast<typename I::pointer>(nullptr);
    else return static_cast<void*>(nullptr);
  }());
  using size = decltype([](auto x) { return sizeof(x); }(I{}));
};
// Not in a template.
struct Plain {
  using value = decltype([] { return 'c'; }());
};
static_assert(same<T<int>::pointer, int*>::value && same<T<char>::pointer, char*>::value);
static_assert(!same<T<int>::closure, T<long>::closure>::value);
static_assert(same<T<int>::closure, T<int>::closure>::value);
static_assert(same<traits<HasPointer>::pointer, long*>::value);
static_assert(same<traits<NoPointer>::pointer, void*>::value);
static_assert(same<traits<NoPointer>::size, decltype(sizeof(0))>::value);
static_assert(same<Plain::value, char>::value);
int main() {
  T<int>::pointer p = nullptr;
  traits<HasPointer>::pointer q = nullptr;
  Plain::value c = 'x';
  return p || q || c != 'x' ? 1 : 0;
}
