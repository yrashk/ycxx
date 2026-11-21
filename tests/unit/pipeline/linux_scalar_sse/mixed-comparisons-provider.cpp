#define COMPARISON_BITS(a, b)                                                  \
  (((a < b) << 0) | ((a <= b) << 1) | ((a > b) << 2) | ((a >= b) << 3) |       \
   ((a == b) << 4) | ((a != b) << 5))

extern "C" int fp_mixed_32_64(float a, double b) {
  return COMPARISON_BITS(a, b);
}

extern "C" int fp_mixed_64_32(double a, float b) {
  return COMPARISON_BITS(a, b);
}
