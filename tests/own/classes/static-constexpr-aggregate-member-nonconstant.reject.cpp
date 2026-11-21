// [dcl.constexpr], [expr.const] (#262)
struct Pt { int a, b; };
int runtime_value();

struct S {
    static constexpr Pt q = Pt{7, runtime_value()};  //! error
};
