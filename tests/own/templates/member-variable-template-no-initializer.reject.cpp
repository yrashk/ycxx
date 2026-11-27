// #340: a `constexpr` static data member template needs an initializer ([class.static.data],
// [dcl.constexpr], [temp.mem]).
struct C {
  template <class T> static constexpr int v;  //! error
};
template <class A> struct D {
  template <class T> static constexpr int w;  //! error
};
int main() { return 0; }
