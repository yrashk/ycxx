#include "held.hpp"

int main() {
  int destroyed = 0;
  {
    Held held = hold(&destroyed);
    if (held.guard.destroyed != &destroyed || destroyed != 0) return 1;
  }
  return destroyed != 1;
}
