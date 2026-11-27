// #157 [class.prop]: ycxx and Clang agree on the layout of classes that contain or derive from a class
// with a const class member, and on one whose member's class has a const-callable assignment.
// PEER: const-member-layout-peer.cpp
// EXIT: 0
#include "const-member-layout.hpp"

int main() {
  for (int i = 0; i < containing; ++i)
    if (containing_size(i) != peer_containing_size(i)) return 1 + i;
  After<Contains> a{{{{1, 2}, 3}, 4}, 5};
  return peer_last(&a) == 5 ? 0 : 20;
}
