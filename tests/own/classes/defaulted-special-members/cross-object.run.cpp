// #157 [class.prop], [class.temporary]: ycxx and Clang agree on the layout of classes with defaulted
// special members and on how they are passed and returned.
// PEER: cross-object-peer.cpp
// XFAIL: gcc its layout of these classes is not the one Clang gives them on this target
// EXIT: 0
#include "cross-object.hpp"

int main() {
  for (int i = 0; i < layouts; ++i)
    if (layout(i) != peer_layout(i)) return 1 + i;
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
