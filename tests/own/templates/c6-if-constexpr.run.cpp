// ζ c6 (#88): if constexpr keeps one branch; the discarded one of a template is not instantiated,
// and outside a template its odr-uses are none ([stmt.if]/2, [basic.def.odr]).
// EXIT: 0
struct S { int v; };
template<class T> constexpr bool integral = false;
template<> constexpr bool integral<char> = true;
template<> constexpr bool integral<int> = true;
template<> constexpr bool integral<unsigned> = true;
template<class T> constexpr bool arith = integral<T>;
template<class T> consteval bool signed_impl() {
  if constexpr (arith<T>)
    return T(-1) < T(0);
  else
    return false;
}
template<class T> int get(const T& t) {
  if constexpr (__is_class(T)) return t.v;
  else if constexpr (__is_pointer(T)) { return *t; }
  else return t;
}
int used_only_discarded();
int main() {
  static_assert(signed_impl<int>() && !signed_impl<unsigned>() && !signed_impl<S>());
  int x = 5;
  if constexpr (sizeof(int) == 4) x += 1; else x += used_only_discarded();
  S s{3};
  return get(s) + get(&x) + get(1) == 10 ? 0 : 1;
}
