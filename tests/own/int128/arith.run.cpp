// #1172 slice 4: native 128-bit arithmetic agrees with constant evaluation. Each operation runs
// over every pair of operands twice, in a constant expression and natively on values read through
// a volatile object, and the digests of the results must be equal; main returns 1 plus the
// operation's number, or 150 and up for the later checks. Division, remainder and the floating conversions call the compiler runtime
// (__divti3 and its siblings).
// FLAGS: -Wno-pedantic
using i128 = __int128;
using u128 = unsigned __int128;

constexpr u128 wide(unsigned long hi, unsigned long lo) { return ((u128)hi << 64) | lo; }

constexpr u128 apply(int k, u128 a, u128 b, int n) {
  i128 sa = (i128)a, sb = (i128)b;
  bool min_over = b == (u128)-1 && a == wide(0x8000000000000000ul, 0);
  if (k == 0) return a + b;
  if (k == 1) return a - b;
  if (k == 2) return a * b;
  if (k == 3) return b != 0 ? a / b : 0;
  if (k == 4) return b != 0 ? a % b : 0;
  if (k == 5) return a & b;
  if (k == 6) return a | b;
  if (k == 7) return a ^ b;
  if (k == 8) return ~a;
  if (k == 9) return -a;
  if (k == 10) return a << n;
  if (k == 11) return a >> n;
  if (k == 12) return a << 64;
  if (k == 13) return a >> 64;
  if (k == 14) return a << 3;
  if (k == 15) return a >> 100;
  if (k == 16) return a < b ? a : b;
  if (k == 17) return (unsigned long)a;
  if (k == 18) return (unsigned char)a;
  if (k == 19) return b != 0 && !min_over ? (u128)(sa / sb) : 0;
  if (k == 20) return b != 0 && !min_over ? (u128)(sa % sb) : 0;
  if (k == 21) return (u128)(sa >> n);
  if (k == 22) return (u128)(sa >> 1);
  if (k == 23) return (u128)(sa >> 64);
  if (k == 24) return (u128)(sa >> 127);
  if (k == 25) return (u128)(i128)(long)a;
  if (k == 26) return (u128)(i128)(int)a;
  if (k == 27) return (u128)(i128)(signed char)a;
  if (k == 28) return (u128)(i128)(short)a;
  if (k == 29) return (unsigned)a;
  if (k == 30) return (u128)(i128)(double)(sa >> 2);
  if (k == 31) return (u128)(i128)(float)(sa >> 2);
  if (k == 32) return (u128)(double)(a >> 1);
  if (k == 33) return (u128)(float)(a >> 1);
  if (k == 34) return a < b;
  if (k == 35) return a <= b;
  if (k == 36) return a > b;
  if (k == 37) return a >= b;
  if (k == 38) return a == b;
  if (k == 39) return a != b;
  if (k == 40) return sa < sb;
  if (k == 41) return sa <= sb;
  if (k == 42) return sa > sb;
  if (k == 43) return sa >= sb;
  if (k == 44) return (bool)a;
  if (k == 45) return !b;
  if (k == 46) return a && b;
  if (k == 47) return (double)a < (double)b;
  if (k == 48) return (float)sa < (float)sb;
  return 0;
}
constexpr int operations = 49;

constexpr u128 values[] = {
    0, 1, 2, 10, wide(0, 0xffffffffffffffff), wide(1, 0), wide(1, 1), wide(0x7fffffffffffffff, 0xffffffffffffffff),
    wide(0x8000000000000000, 0), wide(0xffffffffffffffff, 0xffffffffffffffff), wide(0xffffffffffffffff, 0),
    wide(0x0123456789abcdef, 0xfedcba9876543210), wide(0xdeadbeefcafebabe, 0x0f1e2d3c4b5a6978),
    (u128)-7, wide(0, 0x8000000000000000), wide(0x20000000000001, 0x1),
};
constexpr int count = sizeof(values) / sizeof(values[0]);
constexpr int shifts[] = {0, 1, 31, 63, 64, 65, 100, 127};
constexpr int shift_count = sizeof(shifts) / sizeof(shifts[0]);

volatile u128 input;
volatile int amount;
u128 opaque(u128 v) {
  input = v;
  return input;
}
constexpr u128 operand(u128 v) {
  if consteval {
    return v;
  } else {
    return opaque(v);
  }
}
constexpr int shift(int v) {
  if consteval {
    return v;
  } else {
    amount = v;
    return amount;
  }
}

constexpr u128 digest(int k) {
  u128 d = 1;
  for (int i = 0; i < count; ++i)
    for (int j = 0; j < count; ++j) {
      u128 r = apply(k, operand(values[i]), operand(values[j]), shift(shifts[(i + 3 * j) % shift_count]));
      d = (d ^ r) * 0x9e3779b97f4a7c15ul + (r >> 64);
    }
  return d;
}
template <int K>
constexpr u128 expected = digest(K);

template <int K>
int compare() {
  if constexpr (K < operations) {
    if (digest(K) != expected<K>) return 1 + K;
    return compare<K + 1>();
  } else {
    return 0;
  }
}

struct S {
  char c;
  u128 v;
  char d;
};

int main() {
  if (int e = compare<0>()) return e;
  // Increments and compound assignments, carrying between the halves.
  u128 x = opaque(wide(0, 0xffffffffffffffff));
  if (++x != wide(1, 0)) return 160;
  if (x-- != wide(1, 0) || x != wide(0, 0xffffffffffffffff)) return 161;
  x += opaque(1);
  x *= opaque(wide(1, 0)) + 3;
  if (x != wide(3, 0)) return 162;  // 2^64 * (2^64 + 3), reduced
  x >>= opaque(60);
  x |= 5;
  if (x != 53) return 153;
  i128 y = -(i128)opaque(12345678901234567890u);
  y /= 1000;
  y -= 1;
  if (y != -(i128)12345678901234567 - 1) return 154;
  if ((unsigned long)(opaque(wide(7, 9)) >> 64) != 7) return 155;
  // A pointer offset of 128-bit type and a 128-bit object's members in memory.
  int a[4] = {1, 2, 3, 4};
  if (*(a + (i128)opaque(2)) != 3 || a[opaque(3)] != 4) return 156;
  S s{1, opaque(wide(8, 9)), 2};
  S t = s;
  if (t.v != wide(8, 9) || t.c != 1 || t.d != 2 || alignof(S) != 16 || sizeof(S) != 48) return 157;
  u128 arr[3] = {opaque(1), opaque(wide(2, 2)), 3};
  u128 sum = 0;
  for (u128 v : arr) sum += v;
  if (sum != wide(2, 6)) return 158;
  // Unsigned values at or above 2^127, and the conversions of each floating type.
  if ((double)opaque(wide(0x8000000000000000, 0)) != 170141183460469231731687303715884105728.0) return 159;
  if ((float)opaque(wide(0x8000000000000000, 0)) != 170141183460469231731687303715884105728.0f) return 160;
  volatile double d = 300000000000000000000000000000000000000.0;
  if ((u128)d != wide(0xe1b1e5f90f944d6e, 0x1c00000000000000) && (double)(u128)d != d) return 161;
  volatile float f = -18446744073709551616.0f;
  if ((i128)f != -(i128)wide(1, 0) || (float)(i128)f != f) return 162;
  return 0;
}
