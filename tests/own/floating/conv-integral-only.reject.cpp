// [expr.mul]/2, [expr.shift]/1, [expr.bit.and], [expr.or], [expr.xor], [expr.unary.op]/10,
// [expr.sub]: %, shifts, bitwise operators and ~ need integral (or unscoped enumeration) operands,
// and a subscript an integral one; a floating operand is ill-formed, not converted.
double d = 7.5;
int arr[4];
double a = d % 2; //! error
int b = 1 << d; //! error
double c = d >> 1; //! error
int e = 3 & 1.0; //! error
int f = 2.0f | 1; //! error
int g = 1 ^ d; //! error
int h = ~d; //! error
int i = arr[1.0]; //! error
int ok() { return static_cast<int>(d) % 2 + (1 << static_cast<int>(1.0)); }
