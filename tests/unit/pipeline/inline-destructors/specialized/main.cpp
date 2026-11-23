#include "common.hpp"

int observed;
void run_peer() noexcept;

int main() {
  run_peer();
  if (observed != 27) return 1;
  {
    Cell<int> specialized;
    Cell<char> primary;
  }
  return observed != 2727;
}
