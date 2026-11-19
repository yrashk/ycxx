// [expr.const] (converted constant expression), [dcl.array], [stmt.switch], [dcl.enum],
// [class.bit]: a converted constant expression of integral type allows no floating-integral
// conversion, so a floating array bound, case label, enumerator value or bit-field width is
// ill-formed even when its value is integral; a switch condition cannot be floating.
int a[2.0]; //! error
enum E { e1 = 1.0 }; //! error
struct S { int bits : 3.0; }; //! error
int sw(int x, double y) {
  switch (x) {
  case 1.0: return 1; //! error
  case 2: return 2;
  }
  switch (y) { //! error
  default: return 0;
  }
}
int ok[static_cast<int>(2.0)];
