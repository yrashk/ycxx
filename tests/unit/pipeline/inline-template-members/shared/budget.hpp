#pragma once

// [dcl.inline], [basic.def.odr]: the in-class definition is inline.
template<unsigned Charge> struct Budget {
  unsigned remaining;
  unsigned spend() noexcept {
    remaining -= Charge;
    return remaining;
  }
};
