// #715: the explicit-specifier's expression must be a constant expression; a non-constexpr variable is not ([dcl.fct.spec], [expr.const])
bool flag = true;

struct S {
    int v;
    explicit(flag) S(int x) : v(x) {} //! error
};
