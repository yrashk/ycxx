#pragma once

// [basic.link]: a TU-local type argument makes its specialization TU-local.
template<class Tag> struct LocalBudget {
  unsigned remaining;
  unsigned spend() noexcept {
    remaining -= Tag::charge;
    return remaining;
  }
};
