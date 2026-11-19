// ζ p4 (#88), from the step's review: an explicitly specified pack prefix is extended by
// deduction from a class's arguments and in an explicit specialization ([temp.arg.explicit]/9);
// a pack inside an inner expansion is not the outer one's ([temp.variadic]/5); a block local
// hides a function parameter pack; `>>` closes an empty argument list.
// EXIT: 0
template<class... Ts> int f(Ts... xs) { return 1; }
template<> int f(int, char) { return 2; }
template<> int f<long>(long, long) { return 3; }
template<> int f<>() { return 4; }
int main1() {
  if (f(1, 'c') != 2) return 1;
  if (f(1L, 2L) != 3) return 2;
  if (f() != 4) return 3;
  if (f(1) != 1) return 4;
  return 0;
}
template<class... Ts> struct List2 {};
template<class... Ts> int fl(const List2<Ts...>&) { return sizeof...(Ts); }
template<class... Ts> struct L2 {};
template<class T, class U> struct Pair {};
template<class... As, class... Bs> int g(const L2<Bs...>&, const Pair<As, L2<Bs...>>&... xs) { return sizeof...(As) * 10 + sizeof...(Bs); }
template<class... Ts> int shadow(Ts... xs) {
  int n = sizeof...(xs);
  { int xs = 5; return n + xs; }
}
template<class T = int> struct D {};
template<class T> struct W {};
W<D<>> w;
int main() {
  if (main1() != 0) return 1;
  List2<int, char> l; if (fl<int>(l) != 2) return 2;
  L2<long, short, int> b; Pair<int, L2<long, short, int>> p1; Pair<char, L2<long, short, int>> p2;
  if (g(b, p1, p2) != 23) return 3;
  return shadow(1, 2) == 7 ? 0 : 4;
}
