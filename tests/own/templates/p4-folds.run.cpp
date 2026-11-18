// ζ p4 (#88): every form of fold-expression: unary right and left, binary right and left with
// an initial value, and the values of an empty fold over `&&`, `||` and `,`
// ([expr.prim.fold], [temp.variadic]).
// EXIT: 0
template<class... Ts> bool all(Ts... xs) { return (xs && ...); }
template<class... Ts> bool any(Ts... xs) { return (... || xs); }
template<class... Ts> int minus_right(Ts... xs) { return (xs - ... - 100); }
template<class... Ts> int minus_left(Ts... xs) { return (100 - ... - xs); }
template<class... Ts> int last(Ts... xs) { return (xs , ...); }
template<class... Ts> void none(Ts... xs) { (xs , ...); }
template<class... Ts> int shifts(Ts... xs) { return (1 << ... << xs); }
int main() {
  if (!all() || !all(1, 2) || all(1, 0)) return 1;
  if (any() || !any(0, 3) || any(0, 0)) return 2;
  // 1 - (2 - (3 - 100)) = -98; ((100 - 1) - 2) - 3 = 94.
  if (minus_right(1, 2, 3) != -98 || minus_left(1, 2, 3) != 94) return 3;
  if (last(4, 5, 6) != 6) return 4;
  none();
  if (shifts(1, 2) != 8) return 5;
  return 0;
}
