#include "shapes.hpp"

struct Guard { Three x; unsigned canary; };

static auto deduced(Pair p) { return Pair{p.b, p.a}; }

static int registers() {
  if (sum({20, 22}) != 42 || sum(2L) != -30) return 1;
  if (sum(Pair{20, 22}) != 42) return 2;
  if (padded({0xbe, 0xdeadbeefU}) != (0xbeU ^ 0xdeadbeefU)) return 3;
  Wide w = flip({high, low});
  if (w.a != low || w.b != high) return 4;
  Pair p = deduced({20, 22});
  if (p.a != 22 || p.b != 20) return 5;
  return 0;
}

static int partial_words() {
  Small src[3] = {{1, 2, 3}, {0x81, 0xe3, 0x9a}, {6, 7, 8}};
  src[1] = rotate(src[1]);
  if (src[1].a != 0x9a || src[1].b != 0x81 || src[1].c != 0xe3 ||
      src[2].a != 6 || src[0].c != 3) return 6;
  Guard t = {{-129, 0x23456789, -12345}, 0x9aafe863U};
  t.x = shift(t.x);
  if (t.x.a != -12345 || t.x.b != -127 || t.x.c != 0x23456788 || t.canary != 0x9aafe863U) return 7;
  return 0;
}

static int stack_slots() {
  if (!rollback(1, 2, 3, 4, 5, {high, low}, -6, 7)) return 8;
  Wide w = unused_register(1, 2, 3, 4, 5, {high, low});
  if (w.a != low + 6 || w.b != high + 9) return 9;
  if (!stacked(1, 2, 3, 4, 5, 6, {-9, 117}, {high, low}, 7)) return 10;
  return 0;
}

int main() {
  if (int result = registers()) return result;
  if (int result = partial_words()) return result;
  if (int result = stack_slots()) return result;
  Pair p = {20, 22};
  if (!mutate_parameter(p, &p) || p.a != 20 || !mutate_original(&p, p) || p.a != 44) return 11;
  return 0;
}
