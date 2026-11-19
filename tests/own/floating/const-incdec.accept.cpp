// [expr.pre.incr], [expr.post.incr], [expr.const]: ++ and -- on floating objects add or subtract 1
// in the object's type; the postfix forms yield the old value.
constexpr double incdec() {
  double d = 0.5;
  double old = d++;
  if (old != 0.5 || d != 1.5) return -1;
  if (++d != 2.5) return -2;
  if (d-- != 2.5 || d != 1.5) return -3;
  if (--d != 0.5) return -4;
  return d;
}
static_assert(incdec() == 0.5);
constexpr float float_inc() {
  float f = 16777216.0f;
  ++f;          // 2^24 + 1 is not a float: rounds back to 2^24
  return f;
}
static_assert(float_inc() == 16777216.0f);
constexpr double big_inc() {
  double d = 0x1p53;
  d++;
  return d;
}
static_assert(big_inc() == 0x1p53);
constexpr double small_dec() {
  double d = 0.25;
  --d;
  return d;
}
static_assert(small_dec() == -0.75);
