// [cstdarg.syn], [support.runtime], [expr.call]/12: variadic definitions on Apple arm64 (#647).
// The va_list is a `char*` to the first 8-byte slot after the named stack arguments. ycxx's
// definitions are called from ycxx and from a Clang peer, and a cursor ycxx starts is read by
// the peer's `va_arg`.
// PEER: definitions-peer.cpp
// EXIT: 0
typedef __builtin_va_list va_list;

extern "C" {
long ycxx_sum(int n, ...);
double ycxx_doubles(int n, ...);
long ycxx_stacked(long a, long b, long c, long d, long e, long f, long g, long h, char i, int j, ...);
long ycxx_copied(int n, ...);
long ycxx_forward(int n, ...);
long peer_vsum(int n, va_list ap);
int peer_calls();
double ycxx_fstack(double a, double b, double c, double d, double e, double f, double g, double h, float i,
                   double j, ...);
struct Big { long a, b, c; };
Big ycxx_big(int n, ...);
}

// A va_list parameter consumed by ycxx functions, and copied from a parameter.
static long vsum(int n, va_list ap) {
  long s = 0;
  for (int i = 0; i < n; ++i) s += __builtin_va_arg(ap, long);
  return s;
}

static long vtwice(int n, va_list ap) {
  va_list c;
  __builtin_va_copy(c, ap);
  long a = vsum(n, ap);
  long b = vsum(n, c);
  __builtin_va_end(c);
  return a * 100 + b;
}

static long sum_twice(int n, ...) {
  va_list ap;
  __builtin_va_start(ap, n);
  long r = vtwice(n, ap);
  __builtin_va_end(ap);
  return r;
}

// Eight named doubles fill v0-v7; `i` and `j` go on the stack (bytes 0 and 8), so the first
// unnamed argument is at byte 16.
double ycxx_fstack(double a, double b, double c, double d, double e, double f, double g, double h, float i,
                   double j, ...) {
  va_list ap;
  __builtin_va_start(ap, j);
  int k = __builtin_va_arg(ap, int);
  double x = __builtin_va_arg(ap, double);
  __builtin_va_end(ap);
  return a + b + c + d + e + f + g + h + i + j + k * 100 + x * 1000;
}

// A class result through x8 alongside the ellipsis.
Big ycxx_big(int n, ...) {
  va_list ap;
  __builtin_va_start(ap, n);
  Big b{0, 0, 0};
  b.a = __builtin_va_arg(ap, long);
  b.b = __builtin_va_arg(ap, long) * n;
  b.c = __builtin_va_arg(ap, long) - n;
  __builtin_va_end(ap);
  return b;
}

long ycxx_sum(int n, ...) {
  va_list ap;
  __builtin_va_start(ap, n);
  long s = 0;
  for (int i = 0; i < n; ++i) s += __builtin_va_arg(ap, long);
  __builtin_va_end(ap);
  return s;
}

double ycxx_doubles(int n, ...) {
  va_list ap;
  __builtin_va_start(ap, n);
  double s = 0;
  for (int i = 0; i < n; ++i) s = s * 10 + __builtin_va_arg(ap, double);
  __builtin_va_end(ap);
  return s;
}

// Eight named arguments fill x0-x7; `i` and `j` go on the stack packed by their size (bytes
// 0 and 4), so the first unnamed argument is at byte 8.
long ycxx_stacked(long a, long b, long c, long d, long e, long f, long g, long h, char i, int j, ...) {
  va_list ap;
  __builtin_va_start(ap, j);
  int k = __builtin_va_arg(ap, int);
  double x = __builtin_va_arg(ap, double);
  const char* p = __builtin_va_arg(ap, const char*);
  unsigned long u = __builtin_va_arg(ap, unsigned long);
  __builtin_va_end(ap);
  long named = a + b + c + d + e + f + g + h + i + j;
  return named * 1000 + k * 100 + long(x * 10) + (p[0] == 'y' ? 1 : 50) + (u == 0xfedcba9876543210ul ? 0 : 500);
}

long ycxx_copied(int n, ...) {
  va_list ap, again;
  __builtin_va_start(ap, n);
  __builtin_va_copy(again, ap);
  long first = 0, second = 0;
  for (int i = 0; i < n; ++i) first += __builtin_va_arg(ap, int);
  for (int i = 0; i < n; ++i) second += 2 * __builtin_va_arg(again, int);
  __builtin_va_end(again);
  __builtin_va_end(ap);
  return first * 1000 + second;
}

long ycxx_forward(int n, ...) {
  va_list ap;
  __builtin_va_start(ap, n);
  long r = peer_vsum(n, ap);
  __builtin_va_end(ap);
  return r;
}

int main() {
  if (ycxx_sum(4, 1L, -2L, 3000000000L, 4L) != 3000000003L) return 1;
  if (ycxx_sum(0) != 0) return 2;
  if (ycxx_doubles(3, 1.0, 2.5, -3.0) != 122.0) return 3;
  char text[] = "yes";
  if (ycxx_stacked(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, -7, 2.25, text, 0xfedcba9876543210ul) != 55000 - 700 + 22 + 1)
    return 4;
  if (ycxx_copied(3, 1, -2, 30) != 29 * 1000 + 58) return 5;
  if (ycxx_forward(3, 10L, 20L, -5L) != 25) return 6;
  if (sum_twice(3, 1L, 2L, 3L) != 606) return 7;
  if (ycxx_fstack(1, 2, 3, 4, 5, 6, 7, 8, 0.5f, 0.25, 3, 0.125) != 36.75 + 300 + 125) return 8;
  Big b = ycxx_big(2, 7L, 8L, 9L);
  if (b.a != 7 || b.b != 16 || b.c != 7) return 9;
  return peer_calls();
}
