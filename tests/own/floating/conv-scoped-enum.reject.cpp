// [conv.fpint]/2, [dcl.enum], [expr.static.cast]: a scoped enumeration does not convert implicitly
// to a floating type, nor a floating value implicitly to any enumeration; static_cast does both.
enum class S { one = 1 };
enum U { u1 = 1 };
double a = S::one; //! error
S b = 1.0; //! error
U c = 1.0; //! error
double ok = static_cast<double>(S::one);
S ok2 = static_cast<S>(1.0);
U ok3 = static_cast<U>(1.0);
double ok4 = u1;
