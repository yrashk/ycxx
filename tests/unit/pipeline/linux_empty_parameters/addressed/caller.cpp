#include "transport.hpp"

int observed;
void record(int id) noexcept { observed += id; }
bool same_tag(const Tag *left, const Tag *right) noexcept { return left == right; }

int main() {
  const Word result = transport(1, Tag(3), 2, 3, Tag(9), 4, 5, 6, 7, 8);
  return result == 204 && observed == 12 ? 0 : 1;
}
