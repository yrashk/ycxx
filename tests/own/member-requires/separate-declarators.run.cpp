// #1417 review [class.mem.general]: each member-declarator has its own requires-clause; `g` below
// has none, so its out-of-class definition has none either.
// XFAIL: clang Clang 23 gives the later member-declarator the earlier one's requires-clause (GCC 16 follows [class.mem.general]).
template<class T> struct S {
  int f() requires (sizeof(T) > 100), g();
};
template<class T> int S<T>::g() { return 7; }
int main() { return S<int>{}.g() == 7 ? 0 : 1; }
