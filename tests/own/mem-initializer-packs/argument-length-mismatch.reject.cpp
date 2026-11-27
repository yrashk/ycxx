// #1400 (review): an expansion in a mem-initializer's argument whose packs have different lengths
// ([temp.variadic]: all packs of one expansion have the same number of elements) is one error.
int g(int, int);
template<class... Ts> struct L {};
struct X {
  int m;
  template<class... Ts, class... Us> X(L<Ts...>, Us... us) : m(g(Ts(us)...)) {} //! error
};
X x(L<int, int>{}, 1, 2, 3);
int main() { return 0; }
