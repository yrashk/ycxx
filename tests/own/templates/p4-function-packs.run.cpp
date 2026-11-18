// ζ p4 (#88): function templates with a type parameter pack and a function parameter pack:
// `sizeof...` of both, unary and binary folds, a pack expanded into a call's arguments and into
// a braced list, and nested expansions ([temp.variadic], [expr.prim.fold], [expr.sizeof]).
// EXIT: 0
template<class... Ts>
int count(Ts... xs) { return sizeof...(xs); }
template<class... Ts>
constexpr int types() { return sizeof...(Ts); }
template<class... Ts>
int sum(Ts... xs) { return (xs + ... + 0); }
template<class... Ts>
int sum_left(Ts... xs) { return (... + xs); }
int id(int x) { return x; }
template<class... Ts>
int forward_all(Ts... xs) { return sum(id(xs)...); }
template<class T> T twice(T x) { return x + x; }
template<class... Ts>
int nested(Ts... xs) { return sum(twice(xs)...); }
template<class... Ts>
int first(Ts... xs) { int a[] = {xs...}; return a[0]; }
static_assert(types<int, char, long>() == 3 && types<>() == 0);
int main() {
  if (count() != 0 || count(1, 2, 3) != 3) return 1;
  if (sum(1, 2, 3) != 6 || sum() != 0) return 2;
  if (sum_left(4, 5) != 9) return 3;
  if (forward_all(1, 2, 3, 4) != 10) return 4;
  if (nested(1, 2, 3) != 12) return 5;
  if (first(7, 8) != 7) return 6;
  return 0;
}
