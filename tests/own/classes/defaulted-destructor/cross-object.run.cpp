// #157 [class.dtor], [class.temporary]: the result and parameter have a trivial destructor.
// PEER: cross-object-peer.cpp
// EXIT: 0
#include "cross-object.hpp"

int receiver_calls;

Position* evaluate_receiver(Position* position) {
  ++receiver_calls;
  return position;
}

void destroy_position(Position* position) {
  evaluate_receiver(position)->~Position();
}

int main() {
  Position start{30, 11};
  Position next = advance(start, 4);
  if (next.row != 15 || next.column != 26) return 1;
  if (start.row != 30 || start.column != 11) return 2;
  return check_explicit_destruction();
}
