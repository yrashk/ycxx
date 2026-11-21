#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <xmmintrin.h>

extern "C" {

float fp201_add_f32(float, float);
float fp201_sub_f32(float, float);
float fp201_mul_f32(float, float);
float fp201_div_f32(float, float);
float fp201_neg_f32(float);
float fp201_round_f32(float, float);

double fp201_add_f64(double, double);
double fp201_sub_f64(double, double);
double fp201_mul_f64(double, double);
double fp201_div_f64(double, double);
double fp201_neg_f64(double);
double fp201_round_f64(double, double);

}

namespace {

using U32 = std::uint32_t;
using U64 = std::uint64_t;

// The bit patterns and MXCSR expectations target the Linux x86-64 ABI.
// Mask all exceptions, use nearest-even, and disable DAZ and FTZ.
constexpr unsigned nearest = 0x1f80;
constexpr unsigned downward = nearest | 0x2000;
constexpr unsigned upward = nearest | 0x4000;
constexpr unsigned zero_divide_flag = 0x04;
constexpr unsigned precision_flag = 0x20;

template <typename Float, typename Bits>
Float from_bits(Bits bits) {
  volatile Bits input = bits;
  return std::bit_cast<Float>(static_cast<Bits>(input));
}

template <typename Bits>
bool equal_bits(const char* width, const char* name, const char* field,
                Bits actual, Bits expected) {
  if (actual == expected)
    return true;
  std::fprintf(stderr, "%s %s %s got 0x%llx, expected 0x%llx\n",
               width, name, field, static_cast<unsigned long long>(actual),
               static_cast<unsigned long long>(expected));
  return false;
}

template <typename Float, typename Bits>
struct BinaryCase {
  const char* name;
  Float (*operation)(Float, Float);
  Bits left;
  Bits right;
  Bits expected;
};

constexpr BinaryCase<float, U32> binary32[] = {
    // 6 and 1.5 give exact results, with distinct reversed quotients.
    {"add", fp201_add_f32, 0x40c00000, 0x3fc00000, 0x40f00000},
    {"sub", fp201_sub_f32, 0x40c00000, 0x3fc00000, 0x40900000},
    {"sub reversed", fp201_sub_f32, 0x3fc00000, 0x40c00000, 0xc0900000},
    {"mul", fp201_mul_f32, 0x40c00000, 0x3fc00000, 0x41100000},
    {"div", fp201_div_f32, 0x40c00000, 0x3fc00000, 0x40800000},
    {"div reversed", fp201_div_f32, 0x3fc00000, 0x40c00000, 0x3e800000},
    {"add signed zero", fp201_add_f32, 0x80000000, 0x80000000, 0x80000000},
    {"div signed zero", fp201_div_f32, 0x00000000, 0xc0000000, 0x80000000},
    {"add subnormal inputs", fp201_add_f32, 0x00000001, 0x00000001, 0x00000002},
    {"mul subnormal result", fp201_mul_f32, 0x00800000, 0x3f000000, 0x00400000},
};

constexpr BinaryCase<double, U64> binary64[] = {
    {"add", fp201_add_f64, 0x4018000000000000, 0x3ff8000000000000, 0x401e000000000000},
    {"sub", fp201_sub_f64, 0x4018000000000000, 0x3ff8000000000000, 0x4012000000000000},
    {"sub reversed", fp201_sub_f64, 0x3ff8000000000000, 0x4018000000000000, 0xc012000000000000},
    {"mul", fp201_mul_f64, 0x4018000000000000, 0x3ff8000000000000, 0x4022000000000000},
    {"div", fp201_div_f64, 0x4018000000000000, 0x3ff8000000000000, 0x4010000000000000},
    {"div reversed", fp201_div_f64, 0x3ff8000000000000, 0x4018000000000000, 0x3fd0000000000000},
    {"add signed zero", fp201_add_f64, 0x8000000000000000, 0x8000000000000000, 0x8000000000000000},
    {"div signed zero", fp201_div_f64, 0x0000000000000000, 0xc000000000000000, 0x8000000000000000},
    {"add subnormal inputs", fp201_add_f64, 0x0000000000000001, 0x0000000000000001, 0x0000000000000002},
    {"mul subnormal result", fp201_mul_f64, 0x0010000000000000, 0x3fe0000000000000, 0x0008000000000000},
};

template <typename Float, typename Bits, std::size_t Size>
bool check_binary(const char* width, const BinaryCase<Float, Bits> (&cases)[Size]) {
  _mm_setcsr(nearest);
  bool ok = true;
  for (const auto& test : cases) {
    const Float result = test.operation(from_bits<Float>(test.left), from_bits<Float>(test.right));
    ok &= equal_bits(width, test.name, "bits", std::bit_cast<Bits>(result), test.expected);
  }
  return ok;
}

template <typename Bits>
struct NegationCase {
  const char* name;
  Bits input;
  Bits expected;
};

constexpr NegationCase<U32> negation32[] = {
    {"negate finite", 0x40c00000, 0xc0c00000},
    {"negate positive zero", 0x00000000, 0x80000000},
    {"negate negative zero", 0x80000000, 0x00000000},
    {"negate subnormal", 0x80000001, 0x00000001},
    // The sign flips without changing the payload or quiet bit.
    {"negate quiet NaN", 0x7fc12345, 0xffc12345},
    {"negate signaling NaN", 0xff812345, 0x7f812345},
};

constexpr NegationCase<U64> negation64[] = {
    {"negate finite", 0x4018000000000000, 0xc018000000000000},
    {"negate positive zero", 0x0000000000000000, 0x8000000000000000},
    {"negate negative zero", 0x8000000000000000, 0x0000000000000000},
    {"negate subnormal", 0x8000000000000001, 0x0000000000000001},
    {"negate quiet NaN", 0x7ff8123456789abc, 0xfff8123456789abc},
    {"negate signaling NaN", 0xfff0123456789abc, 0x7ff0123456789abc},
};

template <typename Float, typename Bits, std::size_t Size>
bool check_negation(const char* width, Float (*operation)(Float),
                    const NegationCase<Bits> (&cases)[Size]) {
  constexpr unsigned start = nearest | zero_divide_flag;
  bool ok = true;
  for (const auto& test : cases) {
    _mm_setcsr(start);
    const Float result = operation(from_bits<Float>(test.input));
    const unsigned after = _mm_getcsr();
    ok &= equal_bits(width, test.name, "bits", std::bit_cast<Bits>(result), test.expected);
    ok &= equal_bits(width, test.name, "MXCSR", after, start);
  }
  return ok;
}

template <typename Float, typename Bits>
bool check_rounding(const char* width, Float (*operation)(Float, Float),
                    Bits boundary, Bits one, Bits two, Bits negative_zero) {
  // At 2^24 (float) or 2^53 (double), adding one is a midpoint.
  // The exact cancellation after rounding down has a negative sign.
  const struct {
    const char* name;
    unsigned control;
    Bits expected;
  } cases[] = {
      {"round nearest", nearest, Bits{0}},
      {"round down", downward, negative_zero},
      {"round up", upward, two},
  };
  bool ok = true;
  for (const auto& test : cases) {
    const unsigned start = test.control | zero_divide_flag;
    _mm_setcsr(start);
    const Float result = operation(from_bits<Float>(boundary), from_bits<Float>(one));
    const unsigned after = _mm_getcsr();
    ok &= equal_bits(width, test.name, "bits", std::bit_cast<Bits>(result), test.expected);
    ok &= equal_bits(width, test.name, "MXCSR", after, start | precision_flag);
  }
  return ok;
}

}

int main() {
  const unsigned saved = _mm_getcsr();
  bool ok = check_binary("f32", binary32);
  ok &= check_binary("f64", binary64);
  ok &= check_rounding<float, U32>("f32", fp201_round_f32, 0x4b800000, 0x3f800000,
                                   0x40000000, 0x80000000);
  ok &= check_rounding<double, U64>("f64", fp201_round_f64, 0x4340000000000000,
                                    0x3ff0000000000000, 0x4000000000000000,
                                    0x8000000000000000);
  ok &= check_negation("f32", fp201_neg_f32, negation32);
  ok &= check_negation("f64", fp201_neg_f64, negation64);
  _mm_setcsr(saved);
  return ok ? 0 : 1;
}
