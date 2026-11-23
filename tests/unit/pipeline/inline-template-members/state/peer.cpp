#include "budget.hpp"

unsigned peer_spend(Budget<2u>& budget) noexcept {
  return budget.spend();
}
