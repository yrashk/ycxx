// #119: a class template defined with `class` has private members by default, as a class does
// ([class.access.spec]); its specializations' private members are checked like any class's.
template <class T> class A {
  static const int s = 1;
  T x = 0;
public:
  static const int t = 2;
  T get() const { return x + s; }
};
template <> class A<long> { long y = 0; };
int f(A<int>& a, A<long>& b) {
  int r = A<int>::t + a.get();
  r += A<int>::s;  //! error
  r += a.x;        //! error
  r += int(b.y);   //! error
  return r;
}
int main() { return 0; }
