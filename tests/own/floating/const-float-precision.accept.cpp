// [expr.arith.conv], [basic.fundamental], [expr.const]: float operations are float operations:
// each rounds to float in a constant expression, so adding half a float ulp to 1.0f twice leaves
// 1.0f, while the same additions done in double, then converted, do not.
constexpr float step = 0x1p-24f;
constexpr float in_float = 1.0f + step + step;
constexpr float via_double = static_cast<float>(1.0 + step + step);
static_assert(in_float == 1.0f);
static_assert(via_double == 1.0f + 0x1p-23f);
constexpr float f_third = 1.0f / 3.0f;
static_assert(f_third == 0x1.555556p-2f);
static_assert(f_third != 1.0 / 3.0);
static_assert(static_cast<double>(f_third) == 0x1.555556p-2);
constexpr float sum() {
  float s = 0;
  for (int i = 0; i < 10; ++i)
    s += 0.1f;
  return s;
}
static_assert(sum() == 0x1.000002p0f);
