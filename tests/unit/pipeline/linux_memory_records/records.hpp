// Classes in memory under System V AMD64 (#641): a result of more than two eightbytes, or of
// a class not trivial for calls, goes through a hidden pointer in RDI that comes back in RAX,
// ahead of `this`; a class not trivial for calls passes by the address of its object.
struct Big { long a, b, c, d; };
struct Pair { long a, b; };  // INTEGER, in registers

// Its members are defined out of line in the provider: a constructor defined in its class is
// not yet a weak ELF definition (#422). Everything here is noexcept: a potentially throwing call
// while a `Counted` is alive would need an unwind cleanup, which native code lacks (#89).
struct Counted {
  int v;
  Counted(int x) noexcept;
  Counted(const Counted& o) noexcept;
  ~Counted();
};

struct Maker {
  long k;
  Big make(long x) const noexcept;
  Counted tally(Pair p) const noexcept;
};

Big make_big(long x) noexcept;
Big spill_big(long a, long b, long c, long d, long e, long f) noexcept;
Counted bump(Counted c) noexcept;
int peek(Counted c, long tag) noexcept;
using BigMaker = Big (*)(long) noexcept;
BigMaker big_maker() noexcept;
Big mix(Pair p, long x) noexcept;
Big late(long a, long b, long c, long d, Pair p, long e) noexcept;
long both(Pair p, Counted c, long t) noexcept;
int poke(Counted c) noexcept;
using CountedMaker = Counted (*)(int) noexcept;
CountedMaker counted_maker() noexcept;
