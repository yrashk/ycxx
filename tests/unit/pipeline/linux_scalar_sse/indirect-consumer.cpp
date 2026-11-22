// Host side of indirect-provider.cpp: callees for ycxx's indirect calls.
#include <cstdarg>
#include <cstdio>

extern "C" {
double fp196_apply(double (*)(double, float, int), double, float, int);
float fp196_applyf(float (*)(int, float, long, float), float, float);
double fp196_many(double (*)(double, double, double, double, double, double, double, double, double));
double fp196_variadic(double (*)(int, ...), double, float);
int fp196_variadic_ints(int (*)(int, ...));
}

static double mix(double x, float y, int z) { return x * 100 + y * 10 + z; }
static float mixf(int i, float a, long l, float b) { return i + a * 2 + l + b * 4; }
static double nine(double a, double b, double c, double d, double e, double f, double g, double h, double i) {
  return a + b + c + d + e + f + g + h + i * 1000;
}
static double vsum(int n, ...) {
  va_list ap;
  va_start(ap, n);
  double s = 0;
  for (int k = 0; k < n; ++k) s = s * 10 + va_arg(ap, double);
  va_end(ap);
  return s;
}
static int isum(int n, ...) {
  va_list ap;
  va_start(ap, n);
  int s = 0;
  for (int k = 0; k < n; ++k) s += va_arg(ap, int);
  va_end(ap);
  return s;
}

int main() {
  int bad = 0;
  if (fp196_apply(mix, 1.5, 0.25f, 3) != 155.5) bad |= 1;
  if (fp196_applyf(mixf, 0.5f, 1.25f) != 10.0f) bad |= 2;
  if (fp196_many(nine) != 9536) bad |= 4;
  if (fp196_variadic(vsum, 1, 2.0f) != 122.5) bad |= 8;
  if (fp196_variadic_ints(isum) != 42) bad |= 16;
  if (bad) std::fprintf(stderr, "indirect floating calls: mask %d\n", bad);
  return bad;
}
