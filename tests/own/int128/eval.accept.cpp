// #1172: constant evaluation of __int128 and unsigned __int128 ([expr.const]): arithmetic in 128
// bits ([expr.add], [expr.mul], [expr.shift], [expr.bit.and]), comparisons, and the integral and
// floating-integral conversions ([conv.integral], [conv.fpint]).
// FLAGS: -Wno-pedantic
using i128 = __int128;
using u128 = unsigned __int128;

constexpr u128 umax = ~(u128)0;
constexpr i128 imax = (i128)(umax >> 1);
constexpr i128 imin = -imax - 1;
constexpr i128 big = (i128)1 << 100;

static_assert(umax + 1 == 0 && 0 - (u128)1 == umax);
static_assert((unsigned long long)(umax >> 64) == 0xffffffffffffffffull);
static_assert(imax > 0 && imin < 0 && imin + imax == -1);
static_assert(big > 0 && (big >> 99) == 2 && (big >> 100) == 1);
static_assert(big / 3 * 3 + big % 3 == big);
static_assert(-7 / (i128)2 == -3 && -7 % (i128)2 == -1);
static_assert(umax / 0xffffffffffffffffull == ((u128)1 << 64) + 1);
// Divisors above 2^127.
static_assert(umax / (((u128)1 << 127) + 1) == 1 && umax % (((u128)1 << 127) + 1) == ((u128)1 << 127) - 2);
static_assert((umax - 5) / ((u128)3 << 126) == 1 && (umax - 5) % ((u128)3 << 126) == ((u128)1 << 126) - 6);
static_assert(imin / imin == 1 && imin % imax == -1 && imax / imin == 0 && imin / 2 == -((i128)1 << 126));
static_assert((u128)0xffffffffffffffffull * 0xffffffffffffffffull == umax - (((u128)1 << 65) - 2));
static_assert((i128)-3 * (i128)0x7fffffffffffffffll == -(((i128)3 << 63) - 3));
static_assert(((i128)-1 >> 127) == -1 && (umax >> 127) == 1);
static_assert(((big | 5) & ~(i128)4) == (big | 1) && (big ^ big) == 0);

// Comparisons and the integral conversions.
static_assert((i128)-1 < 0 && (u128)-1 > 0 && (i128)-1 == (u128)-1);
static_assert((long long)((i128)1 << 64 | 5) == 5 && (int)(i128)-1 == -1);
static_assert((i128)(u128)-2 == -2 && (u128)(i128)-2 == umax - 1);
static_assert((i128)-5ll == -5 && (u128)-5ll == umax - 4 && (i128)0xffffffffffffffffull > 0);
static_assert((bool)big && !(bool)(big >> 101));
static_assert((unsigned char)(big + 300) == 44);

// The floating-integral conversions.
static_assert((double)big == 0x1p100 && (float)imin == -0x1p127f);
static_assert((i128)0x1p100 == big && (i128)-0x1.8p70 == -((i128)3 << 69));
static_assert((u128)1e30 == (u128)1000000000000000000ull * 1000000000000ull + 19884624838656ull);
static_assert((double)umax == 0x1p128 && (double)(i128)-1 == -1.0 && (float)(u128)3 == 3.0f);

// Loops and locals, as libycxx's __floor_log10_pow2 computes floor(e * log10(2)).
consteval int floor_log10_pow2(int e) {
  constexpr i128 num = 301029995663981195LL;
  constexpr i128 den = 1000000000000000000LL;
  i128 p = static_cast<i128>(e) * num;
  return static_cast<int>(p >= 0 ? p / den : -((-p + den - 1) / den));
}
static_assert(floor_log10_pow2(1000) == 301 && floor_log10_pow2(-1000) == -302);
static_assert(floor_log10_pow2(0x7fffffff) == 646456992);

constexpr u128 factorial(int n) {
  u128 r = 1;
  for (int k = 2; k <= n; ++k) r *= k;
  return r;
}
static_assert(factorial(34) / factorial(33) == 34 && factorial(34) % 10000000 == 0);
static_assert(factorial(34) / 10000000 % 1000 == 352);

constexpr int count_bits(u128 x) {
  int k = 0;
  for (; x; x &= x - 1) ++k;
  return k;
}
static_assert(count_bits(umax) == 128 && count_bits(big) == 1);

struct Pair {
  i128 a;
  u128 b;
};
constexpr Pair pair{imin, umax};
static_assert(pair.a == imin && pair.b == umax);
constexpr i128 arr[] = {1, big, -big};
static_assert(arr[1] + arr[2] == 0);
