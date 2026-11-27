// #1417 [basic.scope.scope], [over.match.viable], [temp.inst]: members that differ only in their
// trailing requires-clauses are different functions, each with its own definition, whichever is
// declared first. Same-signature copy constructors (as in std::optional) coexist.
template <class T> struct First {
  int f() requires (sizeof(T) > 1) { return 1; }
  int f() { return 2; }
};
template <class T> struct Second {
  int f() { return 2; }
  int f() requires (sizeof(T) > 1) { return 1; }
};
template <class T> struct Copy {
  T v;
  Copy() = default;
  Copy(const Copy&) requires (sizeof(T) == 1) = default;
  Copy(const Copy& o) requires (sizeof(T) > 1) : v(o.v + 1) {}
};
int main() {
  if (First<char>{}.f() != 2 || First<int>{}.f() != 1) return 1;
  if (Second<char>{}.f() != 2 || Second<int>{}.f() != 1) return 2;
  Copy<char> a; a.v = 3; Copy<char> b(a);
  Copy<int> c; c.v = 4; Copy<int> d(c);
  return b.v == 3 && d.v == 5 ? 0 : 3;
}
