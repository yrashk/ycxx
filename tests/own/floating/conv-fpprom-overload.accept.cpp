// [conv.fpprom], [over.ics.scs], [over.ics.rank]: float to double is a floating-point promotion,
// which ranks above the floating-point conversion to long double and the floating-integral
// conversion to int, so a float argument picks f(double).
constexpr int f(double) { return 1; }
constexpr int f(long double) { return 2; }
constexpr int f(int) { return 3; }
constexpr int g(double) { return 1; }
constexpr int g(int) { return 2; }
constexpr int h(long double) { return 1; }
constexpr int h(double) { return 2; }
constexpr float one = 1.0f;
static_assert(f(1.0f) == 1);
static_assert(f(one) == 1);
static_assert(f(1.0) == 1 && f(1.0L) == 2 && f(1) == 3);
static_assert(g(2.5f) == 1);
static_assert(g('a') == 2);  // char to int is an integral promotion
static_assert(h(one) == 2);
