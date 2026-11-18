// ζ p4 (#88), from the step's review: a parenthesized expression with `...` in a template
// argument list or `sizeof...` is a fold only where `op ...` ends it ([expr.prim.fold]); a template
// argument expansion inside an outer expansion of the same pack expands it whole ([temp.variadic]).
// EXIT: 0
template<class... Ts> int cnt() { return sizeof...(Ts); }
template<class... Ts> int sum(Ts... xs) { return (0 + ... + xs); }
template<class T> int one() { return 1; }
template<class... Ts> int g(Ts... xs) { return (cnt<Ts*...>()) + (cnt<const Ts&...>()); }
template<class... Ts> int f2(Ts... xs) { return (sizeof...(Ts) + ... + xs); }
template<class... Ts> int g2(Ts... xs) { return (cnt<Ts...>() + ... + xs); }
template<class... Ts> int h() { return sum((cnt<Ts...>() + one<Ts>())...); }
int main() { return g(1, 2) == 4 && f2(1, 2) == 5 && g2(1, 2) == 5 && h<int, char>() == 6 ? 0 : 1; }
