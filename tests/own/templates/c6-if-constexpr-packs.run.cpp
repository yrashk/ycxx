// ζ c6 (#88): a discarded branch of an instantiation holds pack expansions of its own; those after
// it expand as theirs, and nested if constexpr chooses per specialization ([stmt.if]/2,
// [temp.variadic]).
// EXIT: 0
template<class... Ts> int sum(Ts... xs) { return (0 + ... + xs); }
template<class... Ts> int cnt(Ts...) { return sizeof...(Ts); }
template<class... Ts> int f(Ts... xs) {
  int r = 0;
  if constexpr (sizeof...(Ts) > 2) { r += cnt(xs..., xs...); r += sum((xs * 2)...); }
  else r += 100;
  r += sum((xs + 1)...);
  r += cnt(xs..., 1);
  return r;
}
template<class T> int g(T t) {
  if constexpr (sizeof(T) == 1) return 1;
  else if constexpr (sizeof(T) == 2) return 2;
  else { if constexpr (sizeof(T) == 4) return 4; else return 8; }
}
int main() {
  int a = f(1, 2);       // 100 + 5 + 3 = 108
  int b = f(1, 2, 3);    // 6 + 12 + 9 + 4 = 31
  return a == 108 && b == 31 && g((char)0) == 1 && g((short)0) == 2 && g(0) == 4 && g(0L) == 8 ? 0 : 1;
}
