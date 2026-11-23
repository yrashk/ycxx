// [expr.alignof], [temp.dep.constexpr]
using Size = decltype(sizeof(0));
template<class T, class U> constexpr bool same = false;
template<class T> constexpr bool same<T, T> = true;
template<class T> constexpr bool matches() {
  return same<decltype(alignof(T)), Size>;
}
static_assert(same<decltype(alignof(int)), Size>);
static_assert(matches<int>());
