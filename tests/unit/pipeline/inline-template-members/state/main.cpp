#include "budget.hpp"

unsigned peer_spend(Budget<2u>& budget) noexcept;

int main() {
  Budget<2u> budget{9u};
  if (budget.spend() != 7u) return 1;
  if (peer_spend(budget) != 5u) return 2;
  return budget.remaining != 5u;
}
