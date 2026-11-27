// #1417 review [over.match.viable]/3, [over.over]/5, [expr.prim.req]: a static member named by a
// qualified-id is checked against its constraints when it is called or its address is taken, also
// when it is the only one of its name, and in a requires-expression.
template<class T> struct One {
  static int f() requires (sizeof(T) == 4) { return 1; }
};
template<class T> struct Two {
  static int f() requires (sizeof(T) == 4) { return 1; }
  static int f() requires (sizeof(T) != 4) { return 2; }
};
template<class T> concept HasF = requires { One<T>::f(); };
template<class T> int pick() {
  if constexpr (HasF<T>) return 1;
  else return 2;
}
int main() {
  if (pick<int>() != 1 || pick<char>() != 2) return 1;
  if (One<int>::f() != 1) return 2;
  int (*p)() = &Two<int>::f;
  int (*q)() = Two<char>::f;
  auto r = &Two<int>::f;
  return p() == 1 && q() == 2 && r() == 1 ? 0 : 3;
}
