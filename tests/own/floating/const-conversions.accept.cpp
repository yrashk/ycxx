// [expr.const], [conv.fpint], [conv.double], [conv.fpprom]: conversions between floating and
// integer types, and between floating types, in constant expressions: in initialization, function
// arguments and returns.
constexpr int to_int(double d) { return d; }
constexpr double to_double(int i) { return i; }
constexpr float to_float(double d) { return d; }
constexpr double widen(float f) { return f; }
static_assert(to_int(2.75) == 2 && to_int(-2.75) == -2);
static_assert(to_double(7) / 2 == 3.5);
static_assert(to_float(0.1) == 0.1f);
static_assert(widen(0.1f) == 0x1.99999ap-4);
constexpr long long l = 1e15;
static_assert(l == 1000000000000000LL);
constexpr unsigned char uc = 255.9;
static_assert(uc == 255);
constexpr double from_char = 'a';
static_assert(from_char == 97.0);
constexpr int round_half_up(double d) { return static_cast<int>(d + 0.5); }
static_assert(round_half_up(2.4) == 2 && round_half_up(2.5) == 3);
constexpr int array_size = static_cast<int>(3.9);
int arr[array_size];
static_assert(sizeof(arr) == 3 * sizeof(int));
