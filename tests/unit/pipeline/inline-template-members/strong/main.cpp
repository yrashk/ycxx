#include "budget.hpp"

unsigned peer_spend(Budget<2u>& budget) noexcept;

// [temp.expl.spec], [basic.def.odr]: the specialization has its own inline status.
template<> unsigned Budget<2u>::spend() noexcept {
  remaining -= 3u;
  return remaining;
}

int main() {
  Budget<2u> budget{9u};
  if (budget.spend() != 6u) return 1;
  return peer_spend(budget) != 3u;
}
