#include "records.hpp"

Counted::Counted(int x) noexcept : v(x) {}
Counted::Counted(const Counted& o) noexcept : v(o.v + 1000) {}
Counted::~Counted() {}

Big make_big(long x) noexcept { return Big{x, x * 2, x * 3, x * 4}; }

// The hidden pointer takes RDI, so `f` is the first argument on the stack.
Big spill_big(long a, long b, long c, long d, long e, long f) noexcept { return Big{a + b, c + d, e, f}; }

Big Maker::make(long x) const noexcept { return Big{k, x, k + x, -1}; }

Counted bump(Counted c) noexcept { return Counted(c.v + 1); }

int peek(Counted c, long tag) noexcept { return c.v + int(tag); }

static Big indirect(long x) noexcept { return Big{-x, x, -x, x}; }
BigMaker big_maker() noexcept { return indirect; }

Counted Maker::tally(Pair p) const noexcept { return Counted(int(k + p.a + p.b)); }

// The hidden pointer and the record share the INTEGER registers.
Big mix(Pair p, long x) noexcept { return Big{p.a, p.b, x, p.a + x}; }

// The hidden pointer and four longs leave one register: `p` goes to the stack, `e` takes R9.
Big late(long a, long b, long c, long d, Pair p, long e) noexcept { return Big{a + b + c + d, p.a, p.b, e}; }

long both(Pair p, Counted c, long t) noexcept { return p.a * 100 + p.b * 10 + c.v + t; }

// The callee owns the object it was given the address of; the caller's original is a copy.
int poke(Counted c) noexcept {
  c.v = 1;
  return c.v;
}

static Counted counted(int x) noexcept { return Counted(x * 2); }
CountedMaker counted_maker() noexcept { return counted; }
