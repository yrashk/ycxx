#include "probe.hpp"

int main() {
  int evaluated = 0;
  int value = 42;
  Tag original{evaluated};
  Choice temporary(Tag{evaluated}, &original, static_cast<int&&>(value), 1, 2, 3, 4, 5, 6);
  Choice copied(original, &original, static_cast<int&&>(value), 6, 5, 4, 3, 2, 1);
  return temporary.value != 133 || copied.value != 98 || evaluated != 2;
}
