// #1417 review [over.match.viable]/3, [expr.prim.id.general]/4: naming the only static member of
// that name when its constraints are not satisfied is ill-formed.
template<class T> struct S {
  static int f() requires (sizeof(T) == 4) { return 1; }
};
int main() { return S<char>::f(); } //! error
