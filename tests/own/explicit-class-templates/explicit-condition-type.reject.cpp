// #715 review: an explicit-specifier's condition is a contextually converted constant
// expression of type bool, which admits no boolean conversion from a floating value
// ([dcl.fct.spec], [expr.const.const]).
struct S {
  explicit(1.0) S(char); //! error
  explicit(0) S(int);
};
