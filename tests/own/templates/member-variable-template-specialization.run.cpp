// #340: partial and explicit specializations of a static data member template, in the class and
// at namespace scope ([temp.spec.partial.general]/1, [temp.expl.spec]/2, [temp.mem]).
// XFAIL: ycxx specializations of member variable templates are not supported yet (#1594)
// EXIT: 0
struct N {
  template <class T> static constexpr int v = 1;
  template <class T> static constexpr int v<T*> = 2;
};
template <> constexpr int N::v<int> = 3;
template <class C> struct S {
  template <class T> static constexpr int w = 1;
  template <class T> static constexpr int w<T&> = 2;
};
static_assert(N::v<long> == 1 && N::v<long*> == 2 && N::v<int> == 3);
static_assert(S<int>::w<int> == 1 && S<int>::w<int&> == 2);
int main() {}
