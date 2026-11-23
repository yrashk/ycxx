#include "budget.hpp"

// [dcl.inline]: each in-class member has one function address across TUs.
int main() {
  Address two = &Budget<2u>::initial;
  Address four = &Budget<4u>::initial;
  if (two != peer_two() || four != peer_four()) return 1;
  if (two == four) return 2;
  return two() != 2u || four() != 4u;
}
