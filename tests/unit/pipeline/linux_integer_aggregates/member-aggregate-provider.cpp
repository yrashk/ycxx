#include "member-aggregates.hpp"

Pair Calc::swap(Pair p) const { return Pair{p.b + int(base), p.a}; }

Wide Calc::add(Wide w, long k) {
  base += k;
  return Wide{w.a + base, w.b - base};
}

Three Calc::spill(long a, long b, long c, long d, long e, Three t) const {
  return Three{t.c + int(a + b + c + d + e), t.a, t.b + int(base)};
}

Small Calc::make(unsigned char a) { return Small{a, static_cast<unsigned char>(a + 1), static_cast<unsigned char>(a + 2)}; }

static Pair reverse(Pair p) { return Pair{p.b, p.a}; }
Swapper swapper() { return reverse; }

int apply(Shifter f, Three t, Wide w) { return f(t, w); }

// `this` and four longs leave one register: `w` goes to the stack and `last` takes R9.
long Calc::rollback(long a, long b, long c, long d, Wide w, long last) const {
  return (w.a == high && w.b == low ? 0 : 1000) + a + b + c + d + last * 100 + base;
}

static Wide widen(Wide w) { return Wide{w.b + 1, w.a - 1}; }
Widen widener() { return widen; }
