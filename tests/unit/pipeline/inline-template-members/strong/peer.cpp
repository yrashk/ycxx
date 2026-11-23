#include "budget.hpp"

template<> unsigned Budget<2u>::spend() noexcept {
  remaining -= 3u;
  return remaining;
}

unsigned peer_spend(Budget<2u>& budget) noexcept {
  return budget.spend();
}
