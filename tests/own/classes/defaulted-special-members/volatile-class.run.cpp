// #157 [class.copy.ctor], [class.temporary]/3: ycxx and Clang pass classes with a volatile class
// member by address, in both directions.
// PEER: volatile-class-peer.cpp
// XFAIL: gcc it passes a class whose copy and move are implicitly deleted directly
// EXIT: 0
#include "volatile-class.hpp"

int take(Implicit i) { i.k += 1; return i.k; }
int take(Defaulted d) { d.k += 2; return d.k; }
int take(Derived d) { d.d += 3; return d.k + d.d; }
int take(Member m) { m.d += 4; return m.i.k + m.d; }

int main() {
  if (peer_calls() != 4321) return 1;
  return peer_take(Implicit{}) + 10 * peer_take(Defaulted{}) + 100 * peer_take(Derived{}) + 1000 * peer_take(Member{}) == 4321 ? 0 : 2;
}
