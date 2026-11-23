#include "budget.hpp"

Address peer_two() noexcept { return &Budget<2u>::initial; }
Address peer_four() noexcept { return &Budget<4u>::initial; }
