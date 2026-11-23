#include "budget.hpp"

namespace {
struct Tag { static constexpr unsigned charge = 2u; };
}

unsigned peer_spend(unsigned remaining) noexcept;

int main() {
  LocalBudget<Tag> own{9u};
  if (own.spend() != 7u || own.remaining != 7u) return 1;
  return peer_spend(9u) != 4u;
}
