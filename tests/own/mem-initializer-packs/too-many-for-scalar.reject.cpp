// #1400: an expansion producing two initializers for a scalar member, once instantiated with a
// two-element pack ([dcl.init.general]: a parenthesized expression-list for a non-class
// object shall be a single expression; [dcl.init.list]: a braced list of two for a scalar).
template<class... A> struct Paren {
  int value;
  Paren(A... args) : value(args...) {} //! error
};
template<class... A> struct Braced {
  int value;
  Braced(A... args) : value{args...} {} //! error
};
Paren<int, int> p(1, 2);
Braced<int, int> b(1, 2);
int main() { return 0; }
