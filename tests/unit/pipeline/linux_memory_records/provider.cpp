#include "records.hpp"

Counted::Counted(int x) : v(x) {}
Counted::Counted(const Counted& o) : v(o.v + 1000) {}
Counted::~Counted() {}

Big make_big(long x) { return Big{x, x * 2, x * 3, x * 4}; }

// The hidden pointer takes RDI, so `f` is the first argument on the stack.
Big spill_big(long a, long b, long c, long d, long e, long f) { return Big{a + b, c + d, e, f}; }

Big Maker::make(long x) const { return Big{k, x, k + x, -1}; }

Counted bump(Counted c) { return Counted(c.v + 1); }

int peek(Counted c, long tag) { return c.v + int(tag); }

static Big indirect(long x) { return Big{-x, x, -x, x}; }
BigMaker big_maker() { return indirect; }
