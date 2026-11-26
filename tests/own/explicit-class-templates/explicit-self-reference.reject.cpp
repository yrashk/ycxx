// #715 review: a constructor template whose explicit-specifier asks whether the class converts
// from its argument is not a converting candidate while that question is answered
// ([dcl.fct.spec], [over.match.copy]).
struct S;
int g(S);
template <class U> constexpr bool conv = requires(U u) { g(u); };
struct S {
  template <class U> explicit(!conv<U>) S(U) {}
};
S a = 1; //! error
