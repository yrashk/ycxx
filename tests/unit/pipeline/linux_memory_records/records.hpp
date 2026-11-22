// Classes in memory under System V AMD64 (#641): a result of more than two eightbytes, or of
// a class not trivial for calls, goes through a hidden pointer in RDI that comes back in RAX,
// ahead of `this`; a class not trivial for calls passes by the address of its object.
struct Big { long a, b, c, d; };
struct Pair { long a, b; };  // INTEGER, in registers

// Its members are defined out of line in the provider: a constructor defined in its class is
// not yet a weak ELF definition (#422).
struct Counted {
  int v;
  Counted(int x);
  Counted(const Counted& o);
  ~Counted();
};

struct Maker {
  long k;
  Big make(long x) const;
  Counted tally(Pair p) const;
};

Big make_big(long x);
Big spill_big(long a, long b, long c, long d, long e, long f);
Counted bump(Counted c);
int peek(Counted c, long tag);
using BigMaker = Big (*)(long);
BigMaker big_maker();
Big mix(Pair p, long x);
Big late(long a, long b, long c, long d, Pair p, long e);
long both(Pair p, Counted c, long t);
int poke(Counted c);
using CountedMaker = Counted (*)(int);
CountedMaker counted_maker();
