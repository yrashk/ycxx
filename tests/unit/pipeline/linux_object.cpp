#if defined(__linux__)
static_assert(sizeof(long double) == 16);
#else
static_assert(sizeof(long double) == 8);
#endif

static long bias = 13;
extern "C" long peer(long, long, long, long, long, long, long);

extern "C" long sum7(long a, long b, long c, long d, long e, long f, long g) {
  return a + b + c + d + e + f + g;
}

extern "C" long cross_call(long a) {
  return peer(a, 2, 3, 4, 5, 6, 7) + bias;
}

extern "C" long indirect_call(long (*f)(long), long a) {
  return f(a);
}

extern "C" const char* message() {
  return "ycxx";
}

extern "C" int sum_to(int n) {
  int sum = 0;
  while (n > 0) sum += n--;
  return sum;
}

extern "C" unsigned long long_double_size() {
  return sizeof(long double);
}

long twice(long a) {
  return a + a;
}
