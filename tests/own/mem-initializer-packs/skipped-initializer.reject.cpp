// #1400 (review): an expanded mem-initializer whose id names nothing gets one error, not a second
// one about its pattern ([class.base.init]); an element that is not a direct base is an error.
struct X { int m; template<class... Ts> X(Ts... ts) : nope(ts)... {} }; //! error
struct A { A(int) {} };
struct C { C(int) {} };
struct Y : A { template<class... Ts> Y(Ts... ts) : Ts(ts)... {} }; //! error
Y y(C(1));
int main() { return 0; }
