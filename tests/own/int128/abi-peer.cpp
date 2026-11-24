// The other side of abi.run.cpp, compiled by Clang.
#include "abi.h"

#define OURS(name) peer_##name
#include "abi-defs.h"

int peer_checks() {
  return check_calls(our_first, our_second, our_split, our_stack, our_packed, our_mixed, our_box, our_box_stack,
                     our_box_result, our_signed, our_many, our_seven, our_spills, our_five);
}
