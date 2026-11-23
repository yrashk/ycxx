#include "transport.hpp"

int main() {
  int evaluated = 0;
  int ref = 8;
  const Word result = transport(1, 2, 3, 4, 5, Tag(evaluated), Pair{6, 7},
                                static_cast<int&&>(ref), Byte{9}, 10);
  return result == 385 && evaluated == 1 ? 0 : 1;
}
