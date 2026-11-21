#include <bit>
#include <cstdarg>
#include <cstdint>

extern "C" double read_adjusted_cursor(std::va_list);
extern "C" float return_named_float(float, double, double*, ...);
extern "C" double return_unnamed_double(float, double, ...);

static constexpr std::uint32_t float_nan = 0xffc12345;
static constexpr std::uint32_t float_negative_zero = 0x80000000;
static constexpr std::uint64_t double_nan = 0x7ff8123456789abc;
static constexpr std::uint64_t double_negative_zero = 0x8000000000000000;

static double read_with_host_cursor(int tag, ...) {
  std::va_list cursor;
  va_start(cursor, tag);
  (void)va_arg(cursor, double);
  double result = read_adjusted_cursor(cursor);
  va_end(cursor);
  return result;
}

int main() {
  const float nan32 = std::bit_cast<float>(float_nan);
  const float minus_zero32 = std::bit_cast<float>(float_negative_zero);
  const double nan64 = std::bit_cast<double>(double_nan);
  const double minus_zero64 = std::bit_cast<double>(double_negative_zero);
  double observed[2]{};

  const float returned_nan = return_named_float(nan32, nan64, observed, minus_zero64);
  if (std::bit_cast<std::uint32_t>(returned_nan) != float_nan) return 1;
  if (std::bit_cast<std::uint64_t>(observed[0]) != double_nan ||
      std::bit_cast<std::uint64_t>(observed[1]) != double_negative_zero) return 2;

  const float returned_zero = return_named_float(minus_zero32, 3.25, observed, nan64);
  if (std::bit_cast<std::uint32_t>(returned_zero) != float_negative_zero) return 3;
  if (observed[0] != 3.25 || std::bit_cast<std::uint64_t>(observed[1]) != double_nan) return 4;

  const double host_zero = read_with_host_cursor(0, 5.25, minus_zero64);
  if (std::bit_cast<std::uint64_t>(host_zero) != double_negative_zero) return 5;
  const double host_nan = read_with_host_cursor(0, 5.25, nan64);
  if (std::bit_cast<std::uint64_t>(host_nan) != double_nan) return 6;

  const double returned_double_zero = return_unnamed_double(1.5f, 3.25, minus_zero64);
  if (std::bit_cast<std::uint64_t>(returned_double_zero) != double_negative_zero) return 7;
  const double returned_double_nan = return_unnamed_double(1.5f, 3.25, nan64);
  if (std::bit_cast<std::uint64_t>(returned_double_nan) != double_nan) return 8;
  return 0;
}
