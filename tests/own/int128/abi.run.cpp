// #1172 slice 4: 128-bit integers across calls, as the AMD64 psABI and Apple's arm64 convention
// place them: two registers when two are left, else 16-byte-aligned stack, whole; results in the
// register pair. The Clang peer calls this side and is called by it, in every position, as scalars
// and in a class.
// FLAGS: -Wno-pedantic
// PEER: abi-peer.cpp
#include "abi.h"

u128 wide(unsigned long hi, unsigned long lo) { return ((u128)hi << 64) | lo; }

#define OURS(name) our_##name
#include "abi-defs.h"

int peer_checks();

struct M {
  u128 v;
  u128 add(u128 o, int k) { return v + o + k; }
};

int main() {
  if (int e = check_calls(peer_first, peer_second, peer_split, peer_stack, peer_packed, peer_mixed, peer_box,
                          peer_box_stack, peer_box_result, peer_signed, peer_many, peer_seven, peer_spills, peer_five))
    return e;
  if (int e = check_calls(our_first, our_second, our_split, our_stack, our_packed, our_mixed, our_box,
                          our_box_stack, our_box_result, our_signed, our_many, our_seven, our_spills, our_five))
    return 30 + e;
  if (int e = peer_checks()) return 60 + e;
  // Through a function pointer and a member function.
  M m{wide(1, 2)};
  u128 (*p)(long, u128) = peer_first;
  if (m.add(wide(3, 4), 5) != wide(4, 11) || p(1, wide(2, 3)) != wide(2, 4)) return 100;
  return 0;
}
