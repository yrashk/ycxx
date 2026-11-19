// [expr.assign]/7, [expr.arith.conv]/1, from #336's review: a compound assignment applies the
// usual arithmetic conversions, which make an enumeration with a floating operand ill-formed.
enum E { e1 = 2 };
double d = 1.5;
float f = 2.5f;
E e = e1;
void g() {
  d += e; //! error
  f *= e; //! error
  int i = 3;
  i += e;
}
