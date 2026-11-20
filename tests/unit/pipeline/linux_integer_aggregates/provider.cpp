#include "shapes.hpp"

int sum(Pair x) { return x.a + x.b; }
int sum(long) { return -30; }

Small rotate(Small x) { return {x.c, x.a, x.b}; }
unsigned padded(Padded x) { return x.a ^ x.b; }
Three shift(Three x) { return {x.c, x.a + 2, x.b - 1}; }
Wide flip(Wide x) { return {x.b, x.a}; }

int rollback(long a, long b, long c, long d, long e, Wide x, signed char f, long tail) {
  return a == 1 && b == 2 && c == 3 && d == 4 && e == 5 && f == -6 && tail == 7 &&
         x.a == high && x.b == low;
}

Wide unused_register(long a, long b, long c, long d, long e, Wide x) {
  return {x.b + a + b + c, x.a + d + e};
}

int stacked(long a, long b, long c, long d, long e, long f, Pair p, Wide x, long tail) {
  return a == 1 && b == 2 && c == 3 && d == 4 && e == 5 && f == 6 &&
         p.a == -9 && p.b == 117 && x.a == high && x.b == low && tail == 7;
}

int mutate_parameter(Pair p, const Pair* source) {
  p.a = 9;
  return p.a == 9 && p.b == 22 && source->a == 20;
}

int mutate_original(Pair* source, Pair p) {
  source->a = 44;
  return p.a == 20 && p.b == 22;
}
