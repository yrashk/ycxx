// #157 [class.prop]: the tail padding of a class with a const class member of its own.
// PEER: const-member-layout-peer.cpp
// XFAIL: ycxx@darwin-arm64,gcc@darwin-arm64 not POD for Clang, and POD for ycxx (#1333)
// EXIT: 0
#include "const-member-layout.hpp"

int main() {
  for (int i = 0; i < direct; ++i)
    if (direct_size(i) != peer_direct_size(i)) return 1 + i;
  return 0;
}
