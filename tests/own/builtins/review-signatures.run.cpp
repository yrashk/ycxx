// #211, from the review: transformation built-ins in function templates' signatures, as a return
// type and in a pack expansion whose pattern is therefore a non-deduced context
// ([temp.deduct.type]/5); mangled as Clang does.
// XFAIL: gcc GCC rejects built-in traits in function signatures ("use library traits instead")
// EXIT: 0
template<class E> __underlying_type(E) to_u(E e) { return static_cast<__underlying_type(E)>(e); }
enum class Q : long { q = 8 };
template<class... Ts> struct L {};
template<class... Ts> int g(const L<__remove_cv(Ts)...>&, const L<Ts...>&) { return sizeof...(Ts); }
int main() {
  L<int, char> li;
  L<const int, char> lc;
  return int(to_u(Q::q)) + g(li, lc) == 10 ? 0 : 1;
}
