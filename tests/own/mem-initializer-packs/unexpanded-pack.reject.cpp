// #1400: a pack named in a mem-initializer outside any pack expansion ([temp.variadic]): a
// function parameter pack in a member's parenthesized and braced initializer. A base-class pack
// as a mem-initializer-id without `...` is in base-pack-unexpanded.reject.cpp.
template<class... A> struct M {
  int m;
  M(A... args) : m(args) {} //! error
};
template<class... A> struct N {
  int n;
  N(A... args) : n{args} {} //! error
};
int main() { return 0; }
