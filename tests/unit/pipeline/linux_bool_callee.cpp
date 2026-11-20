#include "linux_bool_abi.hpp"

extern "C" int bool417_arguments(bool first, Wide second, Wide third, Wide fourth,
                               Wide fifth, bool sixth, bool seventh, bool eighth) {
  if (second != 22 || third != 33 || fourth != 44 || fifth != 55) return -1;
  return first + 2 * sixth + 4 * seventh + 8 * eighth;
}

extern "C" bool bool417_return(Wide source) {
  return source;
}

extern "C" int bool417_promoted_returns(int from_false, int from_true) {
  return from_false != 0 || from_true != 1;
}
