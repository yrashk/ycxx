// #157 [class.temporary]: ycxx and Clang agree on how classes with defaulted special members are
// passed and returned. cross-object-layout.run.cpp compares their layouts.
// PEER: cross-object-peer.cpp
// EXIT: 0
#include "cross-object.hpp"

int main() {
  Triple t{};
  t.a = 3;
  t.b = 4;
  Triple s = peer_swap(t);
  if (s.a != 4 || s.b != 3 || t.a != 3) return 20;
  Indirect i(5);
  // One copy for the argument and one for the result.
  Indirect n = peer_next(i);
  if (n.c.v != 7 || n.pad != 6 || i.c.v != 5) return 21;
  MayThrow m;
  m.a = 4;
  if (peer_first(m, 2) != 42) return 23;
  if (peer_volatile(Volatile(6)).x != 7) return 24;
  return peer_sum(i) == 11 ? 0 : 22;
}
