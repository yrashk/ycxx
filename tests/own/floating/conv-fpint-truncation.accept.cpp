// [conv.fpint]/1: a floating value converts to an integer type by truncation (the fractional part
// is discarded); the result is defined whenever the truncated value fits, even when the source
// does not (-0.5 truncates to 0, which unsigned can hold).
static_assert((int)2.9 == 2);
static_assert((int)-2.9 == -2);
static_assert(static_cast<int>(2.5f) == 2);
static_assert(static_cast<long long>(1e10) == 10000000000LL);
static_assert(static_cast<long long>(-9.0e18) == -9000000000000000000LL);
static_assert(static_cast<unsigned>(3.99) == 3u);
static_assert(static_cast<unsigned>(-0.5) == 0u);
static_assert(static_cast<unsigned>(4294967295.9) == 4294967295u);
static_assert(static_cast<unsigned long long>(1.8e19) == 18000000000000000000ULL);
static_assert(static_cast<char>(65.7) == 'A');
static_assert(static_cast<short>(-32768.9) == -32768);
static_assert(static_cast<int>(-0.0) == 0 && static_cast<int>(0x1p-1074) == 0);
static_assert(static_cast<int>(2147483647.5) == 2147483647);
constexpr int truncated = 7.75; // copy-initialization: an implicit conversion, also truncating
static_assert(truncated == 7);
