#include "budget.hpp"

namespace {
struct Tag { static constexpr unsigned charge = 5u; };
}

unsigned peer_spend(unsigned remaining) noexcept {
  LocalBudget<Tag> budget{remaining};
  return budget.spend();
}
