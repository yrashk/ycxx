// #715: the explicit-specifier's expression is a contextually converted constant expression of type bool, so explicit(2) narrows and is ill-formed ([dcl.fct.spec], [expr.const.const])
struct S {
    explicit(2) S(int) {} //! error
    explicit(1) S(long) {}
};
