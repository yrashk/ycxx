// Checks ycxx's conversions (conversions-provider.cpp) against this compiler's own, bit for
// bit, over boundary values, under each MXCSR rounding direction for the inexact ones.
#include <bit>
#include <cfloat>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <xmmintrin.h>

extern "C" {
double fp643_i32_to_f64(int);
double fp643_i64_to_f64(long);
double fp643_u32_to_f64(unsigned);
double fp643_u64_to_f64(unsigned long);
float fp643_i32_to_f32(int);
float fp643_i64_to_f32(long);
float fp643_u32_to_f32(unsigned);
float fp643_u64_to_f32(unsigned long);
double fp643_i16_to_f64(short);
float fp643_u8_to_f32(unsigned char);
double fp643_bool_to_f64(bool);
int fp643_f64_to_i32(double);
long fp643_f64_to_i64(double);
unsigned fp643_f64_to_u32(double);
unsigned long fp643_f64_to_u64(double);
int fp643_f32_to_i32(float);
long fp643_f32_to_i64(float);
unsigned fp643_f32_to_u32(float);
unsigned long fp643_f32_to_u64(float);
short fp643_f64_to_i16(double);
unsigned char fp643_f32_to_u8(float);
float fp643_f64_to_f32(double);
double fp643_round_trip(float);
double fp643_fabs_f64(double);
float fp643_fabs_f32(float);
}

namespace {

int failures = 0;

template <typename T>
T opaque(T value) {
  volatile T v = value;
  return v;
}

template <typename T>
auto bits(T value) {
  if constexpr (sizeof(T) == 8) return std::bit_cast<std::uint64_t>(value);
  else if constexpr (sizeof(T) == 4) return std::bit_cast<std::uint32_t>(value);
  else return static_cast<std::uint64_t>(value);
}

template <typename T>
void same(const char* what, unsigned long long input, T actual, T expected) {
  if (bits(actual) == bits(expected)) return;
  ++failures;
  std::fprintf(stderr, "%s(0x%llx): got 0x%llx, expected 0x%llx\n", what, input,
               static_cast<unsigned long long>(bits(actual)), static_cast<unsigned long long>(bits(expected)));
}

template <typename From, typename To>
void to_float(const char* what, To (*f)(From), From x) {
  same(what, static_cast<unsigned long long>(x), f(x), static_cast<To>(opaque(x)));
}

template <typename From, typename To>
void to_int(const char* what, To (*f)(From), From x) {
  same(what, static_cast<unsigned long long>(bits(x)), f(x), static_cast<To>(opaque(x)));
}

constexpr std::uint64_t u64_inputs[] = {
    0, 1, 2, 3, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff, 0x7fffffff, 0x80000000, 0xffffffff,
    0x100000000, 0x1fffffffffffff, 0x20000000000000, 0x20000000000001, 0x20000000000003,
    0xffffff, 0x1000001, 0x1000003, 0x7ffffffffffffdff, 0x7ffffffffffffe00, 0x7fffffffffffffff,
    0x8000000000000000, 0x8000000000000001, 0x8000000000000400, 0x8000000000000401,
    0x8000000000000c00, 0x8000008000000000, 0x8000008000000001, 0xfffffffffffff400,
    0xfffffffffffffbff, 0xfffffffffffffc00, 0xffffff7fffffffff, 0xffffff8000000000,
    0xfffffffffffffffe, 0xffffffffffffffff, 0x123456789abcdef1, 0xfedcba9876543211};

constexpr double f64_inputs[] = {
    0.0, -0.0, 0.25, 0.5, 0.75, 1.0, 1.5, 2.5, -0.5, -0.75, -1.0, -1.5, -2.5, 255.9, 127.9,
    -128.9, 32767.9, -32768.9, 2147483647.9, -2147483648.9, 4294967295.9, 4294967296.0 - 1,
    9007199254740993.0, 9223372036854774784.0, -9223372036854775808.0, 9223372036854775808.0,
    9223372036854777856.0, 18446744073709549568.0, 1e-300, -1e-300, DBL_MIN, DBL_TRUE_MIN};

void conversions() {
  for (std::uint64_t u : u64_inputs) {
    const auto s64 = static_cast<long>(u);
    const auto u32 = static_cast<unsigned>(u);
    const auto s32 = static_cast<int>(u32);
    to_float("i32->f64", fp643_i32_to_f64, s32);
    to_float("i64->f64", fp643_i64_to_f64, s64);
    to_float("u32->f64", fp643_u32_to_f64, u32);
    to_float("u64->f64", fp643_u64_to_f64, static_cast<unsigned long>(u));
    to_float("i32->f32", fp643_i32_to_f32, s32);
    to_float("i64->f32", fp643_i64_to_f32, s64);
    to_float("u32->f32", fp643_u32_to_f32, u32);
    to_float("u64->f32", fp643_u64_to_f32, static_cast<unsigned long>(u));
    to_float("i16->f64", fp643_i16_to_f64, static_cast<short>(u));
    to_float("u8->f32", fp643_u8_to_f32, static_cast<unsigned char>(u));
  }
  to_float("bool->f64", fp643_bool_to_f64, false);
  to_float("bool->f64", fp643_bool_to_f64, true);

  // Only values the destination type can hold: anything else is undefined ([conv.fpint]/1).
  for (double x : f64_inputs) {
    const float f = static_cast<float>(x);
    if (x > -2147483649.0 && x < 2147483648.0) to_int("f64->i32", fp643_f64_to_i32, x);
    if (x >= -9223372036854775808.0 && x < 9223372036854775808.0) to_int("f64->i64", fp643_f64_to_i64, x);
    if (x > -1.0 && x < 4294967296.0) to_int("f64->u32", fp643_f64_to_u32, x);
    if (x > -1.0 && x < 18446744073709551616.0) to_int("f64->u64", fp643_f64_to_u64, x);
    if (x > -32769.0 && x < 32768.0) to_int("f64->i16", fp643_f64_to_i16, x);
    if (f > -2147483649.0f && f < 2147483648.0f) to_int("f32->i32", fp643_f32_to_i32, f);
    if (f >= -9223372036854775808.0f && f < 9223372036854775808.0f) to_int("f32->i64", fp643_f32_to_i64, f);
    if (f > -1.0f && f < 4294967296.0f) to_int("f32->u32", fp643_f32_to_u32, f);
    if (f > -1.0f && f < 18446744073709551616.0f) to_int("f32->u64", fp643_f32_to_u64, f);
    if (f > -1.0f && f < 256.0f) to_int("f32->u8", fp643_f32_to_u8, f);
    to_float("f64->f32", fp643_f64_to_f32, x);
    to_float("f64->f32", fp643_f64_to_f32, x * 1.0000000596046448);
    same("fabs64", bits(x), bits(fp643_fabs_f64(x)), bits(x) & std::uint64_t{0x7fffffffffffffff});
    same("fabs32", bits(f), bits(fp643_fabs_f32(f)), bits(f) & 0x7fffffffu);
    same("round-trip", bits(f), fp643_round_trip(f), static_cast<double>(f));
  }
  to_float("f64->f32", fp643_f64_to_f32, DBL_MAX);
  to_float("f64->f32", fp643_f64_to_f32, static_cast<double>(FLT_MAX) * 1.5);
  same("fabs64 nan", 0, bits(fp643_fabs_f64(std::bit_cast<double>(0xfff8000000000123ull))), std::uint64_t{0x7ff8000000000123});
  same("fabs32 nan", 0, bits(fp643_fabs_f32(std::bit_cast<float>(0xffc00123u))), std::uint32_t{0x7fc00123});
}

}  // namespace

int main() {
  // MXCSR: all exceptions masked; nearest, down, up and toward zero.
  constexpr unsigned modes[] = {0x1f80u, 0x3f80u, 0x5f80u, 0x7f80u};
  for (unsigned mode : modes) {
    _mm_setcsr(mode);
    conversions();
  }
  _mm_setcsr(0x1f80);
  if (failures) std::fprintf(stderr, "%d conversion mismatches\n", failures);
  return failures != 0;
}
