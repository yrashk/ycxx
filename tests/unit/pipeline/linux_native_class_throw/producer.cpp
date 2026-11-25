#include "types.hpp"

int produce(int value) {
  if (value == 0)
    return 19;
  throw Fault{value};
}

int produce_int(int value) {
  throw value;
}
