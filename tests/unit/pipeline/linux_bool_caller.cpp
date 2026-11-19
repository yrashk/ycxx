#include "linux_bool_abi.hpp"

int main() {
  if (bool417_arguments(false, 22, 33, 44, 55, true, true, false) != 6) return 1;
  if (bool417_arguments(true, 22, 33, 44, 55, false, false, true) != 9) return 2;

  const bool from_zero = bool417_return(0);
  const bool from_high_bit = bool417_return(0x8000000000000000ULL);
  if (bool417_promoted_returns(from_zero, from_high_bit) != 0) return 3;

#ifdef BOOL417_VARIADIC_PROBE
  if (bool417_named(false, 0x1234, true) != 1) return 4;
  if (bool417_named(true, 0x1234, false) != 2) return 5;
#endif

#ifdef BOOL417_CARRIER_PROBE
  if (bool417_dirty_arguments() != 6) return 6;
  const bool dirty_false = bool417_dirty_return(0);
  const bool dirty_true = bool417_dirty_return(1);
  if (bool417_promoted_returns(dirty_false, dirty_true) != 0) return 7;
  if (bool417_argument_bytes(false, 22, 33, 44, 55, true, true, false) != 0) return 8;
  if (bool417_return_bytes() != 0) return 9;
#endif

  return 0;
}
