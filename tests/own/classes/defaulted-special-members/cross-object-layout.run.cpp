// #157 [class.prop]: ycxx and Clang agree on the layout of classes derived from classes with
// defaulted special members, whose POD-for-layout status decides whether their tail padding is reused.
// PEER: cross-object-peer.cpp
// XFAIL: gcc its layout of these classes is not the one Clang gives them on this target
// XFAIL: ycxx@darwin-arm64 spec mode takes the Itanium ABI's C++03 POD rule, Clang's arm64 rule needs -fcompat=clang (#1439)
// PARITY: #1439 own tests cannot compile ycxx in its Clang peer's -fcompat mode
// EXIT: 0
#include "cross-object.hpp"

int main() {
  for (int i = 0; i < layouts; ++i)
    if (layout(i) != peer_layout(i)) return 1 + i;
  return 0;
}
