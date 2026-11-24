// #340 item 2: a member alias template needs its arguments ([temp.names]), names no member it
// does not have, and its arguments must fit its head ([temp.arg]).
struct X {
  template <class U> using apply = U;
};
template <class A> struct Y {
  template <class U> using apply = U;
};
X::apply a = 0;  //! error
X::apply<int, int> b = 0;  //! error
X::missing<int> c = 0;  //! error
typename Y<int>::template missing<int> d = 0;  //! error
int main() { return 0; }
