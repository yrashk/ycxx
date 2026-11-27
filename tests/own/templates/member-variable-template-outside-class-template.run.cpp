// #340: a static data member template of a class template defined outside the class, after both
// template-heads ([temp.mem]/1, [class.static.data]/3).
// XFAIL: ycxx static data members of class templates defined outside the class (#256)
// EXIT: 0
template <class C> struct S {
  template <class T> static const bool v;
  template <class T> static int n;
};
template <class C> template <class T> const bool S<C>::v = sizeof(T) < sizeof(C);
template <class C> template <class T> int S<C>::n = sizeof(T) + sizeof(C);
static_assert(S<long>::v<int> && !S<int>::v<long>);
int main() {
  S<int>::n<char> += 1;
  return S<int>::n<char> == 6 && S<long>::v<char> ? 0 : 1;
}
