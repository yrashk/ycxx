// #1417 review [over.over]/5: the address of a static member whose constraints are not satisfied
// matches no target.
template<class T> struct S {
  static int f() requires (sizeof(T) == 4) { return 1; }
};
int main() {
  int (*q)() = &S<char>::f; //! error
  return q();
}
