#include "records.hpp"

int main() {
  Big b = make_big(5);
  if (b.a != 5 || b.b != 10 || b.c != 15 || b.d != 20) return 1;
  Big s = spill_big(1, 2, 3, 4, 5, 6);
  if (s.a != 3 || s.b != 7 || s.c != 5 || s.d != 6) return 2;
  Maker m{40};
  Big t = m.make(2);
  if (t.a != 40 || t.b != 2 || t.c != 42 || t.d != -1) return 3;
  // A prvalue argument initializes the parameter directly; an lvalue is copied ([dcl.init]).
  Counted fresh = bump(Counted(5));
  if (fresh.v != 6) return 4;
  Counted kept(7);
  if (bump(kept).v != 1008 || kept.v != 7) return 5;
  if (peek(kept, 3) != 1010) return 6;
  Big i = big_maker()(9);
  if (i.a != -9 || i.b != 9 || i.c != -9 || i.d != 9) return 7;
  return 0;
}
