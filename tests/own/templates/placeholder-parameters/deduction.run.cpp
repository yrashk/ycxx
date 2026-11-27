// #1244: a placeholder parameter is deduced from a class template specialization's argument
// with its type ([temp.deduct.type]/8), and its own type is not deduced from a function
// argument ([temp.deduct.type]/5).
// EXIT: 0
template<auto X> struct cw {};
template<auto N> constexpr auto value_of(cw<N>) { return N; }
template<auto N> constexpr int size_of(cw<N>) { return sizeof(N); }
template<auto N> int pick(decltype(N) x) { return N + x; }
int main() {
  static_assert(value_of(cw<7>{}) == 7);
  static_assert(size_of(cw<'a'>{}) == 1 && size_of(cw<1LL>{}) == sizeof(long long));
  return pick<40>(2) == 42 ? 0 : 1;
}
