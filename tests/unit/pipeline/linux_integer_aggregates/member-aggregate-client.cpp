#include "member-aggregates.hpp"

template <class T>
T identity(T value) { return value; }

static int shifted(Three t, Wide w) { return t.a - t.c + (w.a == high && w.b == low ? 0 : 100); }

int main() {
  Calc c{5};
  Pair p = c.swap(Pair{20, 22});
  if (p.a != 27 || p.b != 20) return 1;
  Wide w = c.add(Wide{high, low}, 2);
  if (c.base != 7 || w.a != high + 7 || w.b != low - 7) return 2;
  Three t = c.spill(1, 2, 3, 4, 5, Three{-1, 0x40000000, 9});
  if (t.a != 24 || t.b != -1 || t.c != 0x40000007) return 3;
  Small s = Calc::make(0xfe);
  if (s.a != 0xfe || s.b != 0xff || s.c != 0) return 4;
  Swapper f = swapper();
  Pair q = f(Pair{-3, 4});
  if (q.a != 4 || q.b != -3) return 5;
  if (apply(shifted, Three{50, 0, 8}, Wide{high, low}) != 42) return 6;
  Wide i = identity(Wide{low, high});
  if (i.a != low || i.b != high) return 7;
  auto closure = [k = 3](Pair a) { return Three{a.a, a.b, k}; };
  Three r = closure(Pair{1, 2});
  if (r.a != 1 || r.b != 2 || r.c != 3) return 8;
  return 0;
}
