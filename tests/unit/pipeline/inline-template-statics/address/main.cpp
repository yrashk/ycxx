#include "../profile.hpp"

const unsigned* peer_limit_2() noexcept;
const unsigned* peer_limit_4() noexcept;

// [dcl.inline], [basic.def.odr]: one address per specialization across TUs.
int main() {
  const unsigned* first = &Profile<2u>::limit;
  const unsigned* second = &Profile<4u>::limit;
  if (first != peer_limit_2() || second != peer_limit_4()) return 1;
  if (first == second) return 2;
  return *first != 2u || *second != 4u;
}
