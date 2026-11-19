// [expr.assign]/7, [expr.const]: E1 op= E2 is E1 = E1 op E2 with E1 evaluated once; with an
// integer E1 and a floating E2 the operation is done in the floating type and the result truncated
// back ([conv.fpint]).
constexpr int int_ops() {
  int i = 3;
  i *= 1.5;   // 4.5 -> 4
  if (i != 4) return -1;
  i += 0.75;  // 4.75 -> 4
  if (i != 4) return -2;
  i /= 0.5;   // 8.0
  if (i != 8) return -3;
  i -= 2.5;   // 5.5 -> 5
  return i;
}
static_assert(int_ops() == 5);
constexpr double double_ops() {
  double d = 1;
  d += 2;
  d *= 1.5f;
  d -= 0.5;
  d /= 4;
  return d;
}
static_assert(double_ops() == 1.0);
constexpr float float_ops() {
  float f = 1;
  f += 0.1;   // computed in double, then converted to float
  return f;
}
static_assert(float_ops() == static_cast<float>(1.0 + 0.1));
constexpr unsigned unsigned_ops() {
  unsigned u = 10;
  u *= 0.25;  // 2.5 -> 2
  return u;
}
static_assert(unsigned_ops() == 2u);
