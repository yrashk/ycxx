// #1244: a non-type template parameter declared `auto` takes each argument's type
// ([temp.param]/7, [temp.arg.nontype]/1, [dcl.type.auto.deduct]); inside the template its value
// and `decltype(X)` are those of the argument.
// EXIT: 0
template<auto X> struct cw {
  static constexpr auto value = X;
  using type = decltype(X);
  type get() const { return X; }
};
template<auto N> int get() { return N; }
template<auto N> constexpr auto twice() { return N * 2; }
enum class Color { red, green };
int main() {
  cw<3> c;
  if (c.get() != 3 || cw<'a'>::value != 'a') return 1;
  if (get<3>() + get<'a'>() != 100) return 2;
  if (sizeof(cw<'a'>::type) != 1 || sizeof(cw<3L>::type) != sizeof(long)) return 3;
  static_assert(twice<21>() == 42);
  static_assert(cw<Color::green>::value == Color::green);
  static_assert(cw<true>::value);
  unsigned long long big = cw<~0ULL>::value;
  return big == ~0ULL ? 0 : 4;
}
