#include <string.h>

long sum7(long, long, long, long, long, long, long);
long cross_call(long);
long indirect_call(long (*)(long), long);
const char* message(void);
int sum_to(int);
unsigned long long_double_size(void);
long _Z5twicel(long);

long peer(long a, long b, long c, long d, long e, long f, long g) {
  return a - b + c - d + e - f + g;
}

static long tripled(long a) {
  return a * 3;
}

int main(void) {
  if (sum7(1, -2, 3, 4, 5, 6, 7) != 24) return 1;
  if (cross_call(11) != 27) return 2;
  if (indirect_call(tripled, -9) != -27) return 3;
  if (strcmp(message(), "ycxx") != 0) return 4;
  if (sum_to(5) != 15 || sum_to(-1) != 0) return 5;
  if (long_double_size() != sizeof(long double)) return 6;
  if (_Z5twicel(22) != 44) return 7;
}
