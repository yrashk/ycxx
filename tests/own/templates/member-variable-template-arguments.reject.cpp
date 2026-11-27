// #340: a static data member template's arguments must match its template head
// ([temp.arg], [temp.names], [temp.mem]).
struct C {
  template <class T> static constexpr int v = 1;
  template <int N> static constexpr int n = N;
};
template <class A> struct D {
  template <class T, class U = T> static constexpr int w = 2;
};
int a = C::v<int, int>;  //! error
int b = D<int>::w<int, int, int>;  //! error
int c = C::n<int>;  //! error
int d = C::v<1>;  //! error
int main() { return 0; }
