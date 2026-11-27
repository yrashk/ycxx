// #340 review: a private base's member template named unqualified is inaccessible; a member
// template named without template arguments is an error ([class.access.base], [temp.names]); a
// static data member that is not a template has no second template-head ([temp.mem]/1).
class B { template <int K> static constexpr int v = K; };
struct D : B {
  int f() { return v<1>; }  //! error
};
struct S {
  template <int K> static constexpr int v = K;
  static int g() { return v; }  //! error
};
int a = S::v;  //! error
int b = sizeof(S::v);  //! error
S s;
int c = s.v;  //! error
template <class T> struct C { static const int v; };
template <class T> template <int N> const int C<T>::v = N;  //! error
int main() {}
