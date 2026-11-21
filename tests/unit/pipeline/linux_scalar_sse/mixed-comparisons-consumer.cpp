extern "C" int fp_mixed_32_64(float, double);
extern "C" int fp_mixed_64_32(double, float);
extern "C" int printf(const char*, ...);

// Compile consumer.cpp with -Dmain=fp_same_width_main when linking this probe.
int fp_same_width_main();

namespace {

constexpr unsigned ControlMask = 0xffc0U;
constexpr int Less = (1 << 0) | (1 << 1) | (1 << 5);
constexpr int Greater = (1 << 2) | (1 << 3) | (1 << 5);

const volatile unsigned int lower_bits = 0x3f800000U;  // 1
const volatile unsigned int upper_bits = 0x3f800001U;  // 1 + 2^-23
const volatile unsigned long long midpoint_bits = 0x3ff0000010000000ULL;  // 1 + 2^-24

float load32(unsigned int bits) {
  float value;
  __builtin_memcpy(&value, &bits, sizeof(value));
  return value;
}

double load64(unsigned long long bits) {
  double value;
  __builtin_memcpy(&value, &bits, sizeof(value));
  return value;
}

unsigned mxcsr_controls() {
  unsigned int mxcsr;
  __asm__ __volatile__("stmxcsr %0" : "=m"(mxcsr) : : "memory");
  return mxcsr & ControlMask;
}

int check_order(const char* label, int actual, int expected) {
  if (actual == expected) return 0;
  printf("%s got %d, expected %d\n", label, actual, expected);
  return 1;
}

int check_controls(const char* label, unsigned actual, unsigned expected) {
  if (actual == expected) return 0;
  printf("MXCSR controls after %s got %#x, expected %#x\n", label, actual, expected);
  return 1;
}

}  // namespace

int main() {
  const unsigned before = mxcsr_controls();
  int failures = fp_same_width_main();
  const unsigned after_same = mxcsr_controls();

  const float lower = load32(lower_bits);
  const float upper = load32(upper_bits);
  const double midpoint = load64(midpoint_bits);
  const int lower_forward = fp_mixed_32_64(lower, midpoint);
  const int lower_reverse = fp_mixed_64_32(midpoint, lower);
  const int upper_forward = fp_mixed_32_64(upper, midpoint);
  const int upper_reverse = fp_mixed_64_32(midpoint, upper);
  const unsigned after_mixed = mxcsr_controls();

  failures += check_order("f32 lower vs f64 midpoint", lower_forward, Less);
  failures += check_order("f64 midpoint vs f32 lower", lower_reverse, Greater);
  failures += check_order("f32 upper vs f64 midpoint", upper_forward, Greater);
  failures += check_order("f64 midpoint vs f32 upper", upper_reverse, Less);
  failures += check_controls("same-width comparisons", after_same, before);
  failures += check_controls("mixed-width comparisons", after_mixed, after_same);
  return failures != 0;
}
