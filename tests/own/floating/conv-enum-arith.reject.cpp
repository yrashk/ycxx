// [expr.arith.conv]: if one operand has enumeration type and the other a floating-point type, the
// expression is ill-formed (for arithmetic and comparison alike); an explicit cast is fine.
enum E { e1 = 1 };
E e = e1;
double a = e + 1.0; //! error
double b = 2.0f * e1; //! error
bool c = e < 0.5; //! error
bool d = 1.5 == e1; //! error
double ok = static_cast<int>(e) + 1.0;
int ok2 = e + 1;
